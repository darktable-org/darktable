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

// What the panel decides to *show*, as opposed to what it does to the mask.
//
// These are display rules -- which warning badge a row carries, which sliders a
// parametric row exposes, what the panel preferences are -- and every one of
// them is a decision the panel makes from values it can read, ahead of touching
// any widget. Only the decisions are tested; the widget updates they drive are
// on the manual checklist in README.md.
//
// Where a rule lives inside a widget function, it has been split out into a
// `_model_*` decision the widget code then applies, so the rule and the panel
// cannot disagree.

#include "flexi_fixture.h"
#include "control/conf.h"

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
// the no-op predicate behind the no-op badge
// ---------------------------------------------------------------------------

// build a single-channel parametric form whose sub-ranges are all at the full
// default span -- i.e. it restricts nothing
static dt_masks_form_t *_make_parametric(void)
{
  dt_masks_form_t *f = calloc(1, sizeof(dt_masks_form_t));
  f->formid = 50;
  f->type = DT_MASKS_PARAMETRIC;
  dt_masks_point_parametric_t *p = calloc(1, sizeof(dt_masks_point_parametric_t));
  p->colorspace = DEVELOP_BLEND_CS_RGB_SCENE;
  p->channel = 0;
  for(int i = 0; i < 4 * DEVELOP_BLENDIF_SIZE; i += 4)
  {
    p->blendif_parameters[i + 0] = 0.0f;
    p->blendif_parameters[i + 1] = 0.0f;
    p->blendif_parameters[i + 2] = 1.0f;
    p->blendif_parameters[i + 3] = 1.0f;
  }
  f->points = g_list_append(NULL, p);
  return f;
}

static void _free_parametric(dt_masks_form_t *f)
{
  g_list_free_full(f->points, free);
  free(f);
}

static void test_untouched_parametric_is_a_noop(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  assert_true(dt_masks_parametric_is_noop(f, FALSE));
  _free_parametric(f);
}

static void test_narrowed_parametric_is_not_a_noop(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);
  dt_masks_point_parametric_t *p = f->points->data;
  // narrow the input sub-range of the form's own channel
  p->blendif_parameters[4 * ch[0].param_channels[0] + 2] = 0.5f;

  assert_false(dt_masks_parametric_is_noop(f, FALSE));
  _free_parametric(f);
}

// an output sub-range still refines the mask even while its slider is hidden,
// so it counts too -- not just whichever one the UI happens to show
static void test_output_range_alone_is_not_a_noop(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);
  dt_masks_point_parametric_t *p = f->points->data;
  p->blendif_parameters[4 * ch[0].param_channels[1] + 2] = 0.5f;

  assert_false(dt_masks_parametric_is_noop(f, FALSE));
  _free_parametric(f);
}

// inversion is excluded outright: a full range selects everything, its
// complement selects nothing -- a different kind of wrong, not a no-op. The
// member holding the form inverts it, as the panel's invert does
static void test_inverted_parametric_is_never_a_noop(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  assert_false(dt_masks_parametric_is_noop(f, TRUE));
  _free_parametric(f);
}

// a range with its polarity bit set, as a migrated channel can have, inverts
// that range: at its full span it selects nothing
static void test_negative_polarity_is_never_a_noop(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);
  dt_masks_point_parametric_t *p = f->points->data;
  p->blendif |= 1u << (ch[0].param_channels[0] + 16);

  assert_false(dt_masks_parametric_is_noop(f, FALSE));
  _free_parametric(f);
}

// a drawn shape is not a parametric form and never carries the no-op badge
static void test_a_shape_is_never_a_noop(void **state)
{
  flexi_build("u{1,2}");
  assert_false(dt_masks_parametric_is_noop(dt_masks_get_from_id(&flexi_dev, 1), FALSE));
}

// ---------------------------------------------------------------------------
// adaptive display of a parametric row
// ---------------------------------------------------------------------------

#define AUTO DT_MASKS_PARAM_VIEW_AUTO
#define INPUT DT_MASKS_PARAM_VIEW_INPUT
#define OUTPUT DT_MASKS_PARAM_VIEW_OUTPUT
#define BOTH DT_MASKS_PARAM_VIEW_BOTH

// the input slider is always there, and the output one is the toggle's choice,
// whatever the user has touched. A row that held the output alone shows both
static void test_chosen_view_decides_the_sliders(void **state)
{
  for(int out_used = 0; out_used < 2; out_used++)
  {
    const dt_masks_param_vis_t i =
      dt_masks_model_param_row_visibility(INPUT, out_used, FALSE, FALSE);
    assert_true(i.input);
    assert_false(i.output);
    const dt_masks_param_vis_t o =
      dt_masks_model_param_row_visibility(OUTPUT, out_used, FALSE, FALSE);
    assert_true(o.input);
    assert_true(o.output);
    const dt_masks_param_vis_t b =
      dt_masks_model_param_row_visibility(BOTH, out_used, FALSE, FALSE);
    assert_true(b.input);
    assert_true(b.output);
  }
}

// the boost factor shows under the sliders when asked to, and nowhere else,
// and only for a channel that has one
static void test_boost_shows_only_with_the_sliders(void **state)
{
  assert_true(dt_masks_model_param_row_visibility(AUTO, TRUE, TRUE, TRUE).boost);
  assert_false(dt_masks_model_param_row_visibility(AUTO, TRUE, TRUE, FALSE).boost);
  assert_false(dt_masks_model_param_row_visibility(AUTO, TRUE, FALSE, TRUE).boost);
}

// with nothing chosen, a row adapts: it shows the output slider only once its
// range is in use, rather than a second slider that says nothing
static void test_automatic_row_follows_the_output_range(void **state)
{
  const dt_masks_param_vis_t unused =
    dt_masks_model_param_row_visibility(AUTO, FALSE, FALSE, FALSE);
  assert_true(unused.input);
  assert_false(unused.output);
  const dt_masks_param_vis_t used =
    dt_masks_model_param_row_visibility(AUTO, TRUE, FALSE, FALSE);
  assert_true(used.input);
  assert_true(used.output);
}

// the automatic choice resolves to the sliders it shows, and a chosen one to
// itself, but for the output alone, which is both
static void test_view_resolve(void **state)
{
  assert_int_equal(dt_masks_model_param_view_resolve(AUTO, FALSE), INPUT);
  assert_int_equal(dt_masks_model_param_view_resolve(AUTO, TRUE), BOTH);
  assert_int_equal(dt_masks_model_param_view_resolve(OUTPUT, FALSE), BOTH);
  assert_int_equal(dt_masks_model_param_view_resolve(INPUT, TRUE), INPUT);
}

// the swatch shows the colors the element selects: normally the one stretch
// where the range has opacity, inverted the two outside it, with an empty one
// left out
static void test_swatch_span(void **state)
{
  const float r[4] = { 0.2f, 0.3f, 0.6f, 0.8f };
  dt_masks_swatch_span_t span = dt_masks_model_param_swatch_span(r, FALSE);
  assert_int_equal(span.n, 1);
  assert_float_equal(span.from[0], 0.2f, 1e-6);
  assert_float_equal(span.to[0], 0.8f, 1e-6);

  span = dt_masks_model_param_swatch_span(r, TRUE);
  assert_int_equal(span.n, 2);
  assert_float_equal(span.from[0], 0.0f, 1e-6);
  assert_float_equal(span.to[0], 0.3f, 1e-6);
  assert_float_equal(span.from[1], 0.6f, 1e-6);
  assert_float_equal(span.to[1], 1.0f, 1e-6);

  // markers at the ends: nothing lies outside them on that side
  const float left[4] = { 0.0f, 0.0f, 0.5f, 0.7f };
  span = dt_masks_model_param_swatch_span(left, TRUE);
  assert_int_equal(span.n, 1);
  assert_float_equal(span.from[0], 0.5f, 1e-6);
  assert_float_equal(span.to[0], 1.0f, 1e-6);

  // a channel at its full range, inverted, selects nothing
  const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
  assert_int_equal(dt_masks_model_param_swatch_span(full, TRUE).n, 0);
  assert_int_equal(dt_masks_model_param_swatch_span(full, FALSE).n, 1);
}

// the edges of the range fade as its markers say, and inverted it is what is
// left
static void test_swatch_alpha(void **state)
{
  const float r[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, FALSE, 0.1f), 0.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, FALSE, 0.3f), 0.5f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, FALSE, 0.5f), 1.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, FALSE, 0.7f), 0.5f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, FALSE, 0.9f), 0.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, TRUE, 0.1f), 1.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, TRUE, 0.3f), 0.5f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(r, TRUE, 0.5f), 0.0f, 1e-6);

  // a hard edge, with no fade to divide by
  const float hard[4] = { 0.3f, 0.3f, 0.7f, 0.7f };
  assert_float_equal(dt_masks_model_param_swatch_alpha(hard, FALSE, 0.29f), 0.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(hard, FALSE, 0.31f), 1.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(hard, FALSE, 0.69f), 1.0f, 1e-6);
  assert_float_equal(dt_masks_model_param_swatch_alpha(hard, FALSE, 0.71f), 0.0f, 1e-6);
}

// the view and the boost bit share the field, and neither moves the other. The
// bit a row's expansion used to be is left as found
static void test_view_and_boost_share_in_out(void **state)
{
  dt_masks_point_parametric_t p = { .in_out = DT_MASKS_PARAM_EXPANDED };
  dt_masks_param_set_view(&p, OUTPUT);
  dt_masks_param_set_boost_with_sliders(&p, TRUE);
  assert_int_equal(dt_masks_param_get_view(&p), OUTPUT);
  assert_true(dt_masks_param_boost_with_sliders(&p));
  dt_masks_param_set_view(&p, BOTH);
  assert_true(dt_masks_param_boost_with_sliders(&p));
  dt_masks_param_set_boost_with_sliders(&p, FALSE);
  assert_int_equal(dt_masks_param_get_view(&p), BOTH);
  assert_true(p.in_out & DT_MASKS_PARAM_EXPANDED);
}

#undef AUTO
#undef INPUT
#undef OUTPUT
#undef BOTH

// ---------------------------------------------------------------------------
// the "is this sub-range used" predicate
// ---------------------------------------------------------------------------

static void test_channel_used_detects_a_touched_range(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  dt_masks_point_parametric_t *p = f->points->data;
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);

  assert_false(dt_masks_gui_param_channel_is_used(p, &ch[0], 0));
  p->blendif_parameters[4 * ch[0].param_channels[0] + 2] = 0.5f;
  assert_true(dt_masks_gui_param_channel_is_used(p, &ch[0], 0));
  _free_parametric(f);
}

// the channel's own enable bit counts as "used" even at a full range, so a
// channel the user explicitly switched on does not read as untouched
static void test_channel_used_honors_the_active_bit(void **state)
{
  dt_masks_form_t *f = _make_parametric();
  dt_masks_point_parametric_t *p = f->points->data;
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);

  assert_false(dt_masks_gui_param_channel_is_used(p, &ch[0], 0));
  p->blendif |= (1u << ch[0].param_channels[0]);
  assert_true(dt_masks_gui_param_channel_is_used(p, &ch[0], 0));
  _free_parametric(f);
}

// ---------------------------------------------------------------------------
// panel preferences
// ---------------------------------------------------------------------------

static int _conf_setup(void **state)
{
  flexi_conf_init();
  return 0;
}

static int _conf_teardown(void **state)
{
  flexi_teardown();
  flexi_conf_cleanup();
  return 0;
}

// "sticky opacity" off means a new shape's opacity is remembered; on means the
// stored opacity resets to full after each use, so the next shape starts opaque
static void test_sticky_opacity_preference_roundtrips(void **state)
{
  dt_conf_set_bool("plugins/darkroom/masks/opacity_not_sticky", FALSE);
  assert_false(dt_conf_get_bool("plugins/darkroom/masks/opacity_not_sticky"));

  dt_conf_set_float("plugins/darkroom/masks/opacity", 0.4f);
  assert_float_equal(dt_conf_get_float("plugins/darkroom/masks/opacity"), 0.4f, 1e-6);

  // with stickiness disabled the panel resets the stored value to 1.0 after a
  // shape is created (see dt_masks_form_gui_t's opacity handling in masks.c)
  dt_conf_set_bool("plugins/darkroom/masks/opacity_not_sticky", TRUE);
  if(dt_conf_get_bool("plugins/darkroom/masks/opacity_not_sticky"))
    dt_conf_set_float("plugins/darkroom/masks/opacity", 1.0f);
  assert_float_equal(dt_conf_get_float("plugins/darkroom/masks/opacity"), 1.0f, 1e-6);
}

// ---------------------------------------------------------------------------
// the panel options popover's dependent checks
// ---------------------------------------------------------------------------

// the resize handle is the canvas panel's alone; the focus row is for both
// hosted positions, and an embedded panel has neither. The popover builds
// these from the stored position and updates them on a change, from the same
// functions, so this table is both
static void test_position_options_follow_the_position(void **state)
{
  assert_false(dt_masks_gui_pref_always_on_applies(MASKS_PANEL_POS_EMBEDDED));
  assert_false(dt_masks_gui_pref_always_on_applies(MASKS_PANEL_POS_UTILITY));
  assert_true(dt_masks_gui_pref_always_on_applies(MASKS_PANEL_POS_CANVAS));

  assert_false(dt_masks_gui_pref_handle_applies(MASKS_PANEL_POS_EMBEDDED));
  assert_false(dt_masks_gui_pref_handle_applies(MASKS_PANEL_POS_UTILITY));
  assert_true(dt_masks_gui_pref_handle_applies(MASKS_PANEL_POS_CANVAS));

  assert_false(dt_masks_gui_pref_focus_row_applies(MASKS_PANEL_POS_EMBEDDED));
  assert_true(dt_masks_gui_pref_focus_row_applies(MASKS_PANEL_POS_UTILITY));
  assert_true(dt_masks_gui_pref_focus_row_applies(MASKS_PANEL_POS_CANVAS));
}

// a position the focus row's host does not exist for must not carry the handle
static void test_handle_implies_focus_row(void **state)
{
  for(int pos = MASKS_PANEL_POS_EMBEDDED; pos <= MASKS_PANEL_POS_CANVAS; pos++)
    if(dt_masks_gui_pref_handle_applies(pos))
      assert_true(dt_masks_gui_pref_focus_row_applies(pos));
}

// ---------------------------------------------------------------------------
// section fold states
// ---------------------------------------------------------------------------

// nothing saved yet: every section starts unfolded, from the defaults in
// darktableconfig.xml. The controls section's state is its member's (below)
static void test_sections_start_expanded(void **state)
{
  for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
    if(s != DT_MASKS_SECTION_CONTROLS) assert_true(dt_masks_model_section_expanded(s));
  for(dt_masks_controls_kind_t k = 0; k < DT_MASKS_CONTROLS_COUNT; k++)
    assert_true(dt_masks_model_controls_expanded(k, FALSE));
}

// each section keeps its own state: folding one leaves the others alone
static void test_sections_fold_independently(void **state)
{
  for(dt_masks_section_t folded = 0; folded < DT_MASKS_SECTION_COUNT; folded++)
  {
    if(folded == DT_MASKS_SECTION_CONTROLS) continue;
    dt_masks_model_section_save(folded, FALSE);
    for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
      if(s != DT_MASKS_SECTION_CONTROLS)
        assert_int_equal(dt_masks_model_section_expanded(s), s != folded);
    dt_masks_model_section_save(folded, TRUE);
    for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
      if(s != DT_MASKS_SECTION_CONTROLS) assert_true(dt_masks_model_section_expanded(s));
  }
}

// the state lives in the config, not in any module's panel, so it is the same
// for every module and survives a restart
static void test_section_state_is_one_config_key_each(void **state)
{
  dt_masks_model_section_save(DT_MASKS_SECTION_REFINE, FALSE);
  dt_masks_model_section_save(DT_MASKS_SECTION_HELP, FALSE);
  dt_masks_model_section_save(DT_MASKS_SECTION_CONSUMERS, FALSE);
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/refinements_collapsed"));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/help_collapsed"));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/consumers_collapsed"));

  dt_conf_set_bool("plugins/darkroom/masks/consumers_collapsed", FALSE);
  assert_true(dt_masks_model_section_expanded(DT_MASKS_SECTION_CONSUMERS));
}

// the controls subpanel keeps a state per kind of member: folding it for one
// leaves the other alone
static void test_controls_fold_per_member_kind(void **state)
{
  dt_masks_model_controls_save(DT_MASKS_CONTROLS_PARAMETRIC, FALSE);
  assert_false(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_PARAMETRIC, FALSE));
  assert_true(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_SHAPE, FALSE));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/parametric_controls_collapsed"));
  assert_false(dt_conf_get_bool("plugins/darkroom/masks/shape_controls_collapsed"));
  dt_masks_model_controls_save(DT_MASKS_CONTROLS_PARAMETRIC, TRUE);

  dt_masks_model_controls_save(DT_MASKS_CONTROLS_SHAPE, FALSE);
  assert_false(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_SHAPE, FALSE));
  assert_true(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_PARAMETRIC, FALSE));
  dt_masks_model_controls_save(DT_MASKS_CONTROLS_SHAPE, TRUE);
}

// drawing a shape opens its creation controls in a folded controls subpanel,
// and does not save that: once the drawing ends, the subpanel folds back
static void test_drawing_opens_the_controls_without_saving(void **state)
{
  dt_masks_model_controls_save(DT_MASKS_CONTROLS_SHAPE, FALSE);
  assert_true(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_SHAPE, TRUE));
  assert_false(dt_masks_model_controls_expanded(DT_MASKS_CONTROLS_SHAPE, FALSE));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/shape_controls_collapsed"));
  dt_masks_model_controls_save(DT_MASKS_CONTROLS_SHAPE, TRUE);
}

// the mask panel position drives where the panel is hosted; the values the
// panel switches on must round-trip as integers
static void test_panel_position_preference_roundtrips(void **state)
{
  const int positions[] = { MASKS_PANEL_POS_EMBEDDED, MASKS_PANEL_POS_UTILITY,
                            MASKS_PANEL_POS_CANVAS };
  for(size_t i = 0; i < G_N_ELEMENTS(positions); i++)
  {
    dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", positions[i]);
    assert_int_equal(dt_conf_get_int("plugins/darkroom/blend/masks_panel_position"),
                     positions[i]);
  }
}

// which edge the panel docks against. Until the user has pinned it once the key
// does not exist, and the panel must land next to the processing modules rather
// than wherever an unset boolean happens to read.
static void test_panel_side_defaults_to_the_processing_modules_side(void **state)
{
  dt_conf_remove_key("plugins/darkroom/blend/masks_panel_side_right");

  // processing modules live on the right by default
  dt_conf_set_bool("plugins/darkroom/panel_swap", FALSE);
  assert_true(dt_masks_gui_panel_side_right());

  // ... and on the left once the two side panels are swapped
  dt_conf_set_bool("plugins/darkroom/panel_swap", TRUE);
  assert_false(dt_masks_gui_panel_side_right());
}

// once the user has pinned the panel somewhere, that choice outranks the
// processing-modules side: the panel stays where it was put, whatever
// panel_swap says afterwards
static void test_pinned_panel_side_outranks_the_default(void **state)
{
  dt_conf_set_bool("plugins/darkroom/panel_swap", FALSE);
  dt_conf_set_bool("plugins/darkroom/blend/masks_panel_side_right", FALSE);
  assert_false(dt_masks_gui_panel_side_right());

  dt_conf_set_bool("plugins/darkroom/panel_swap", TRUE);
  dt_conf_set_bool("plugins/darkroom/blend/masks_panel_side_right", TRUE);
  assert_true(dt_masks_gui_panel_side_right());
}

// ---------------------------------------------------------------------------
// mask panel & corner icon state transitions
// ---------------------------------------------------------------------------

// when a module is focused and masking-capable, but collapsed:
// - the panel is collapsed
// - the corner icon is visible
// - the corner icon active status reflects whether masking is on or off
static void test_panel_state_collapsed_module_shows_corner_icon(void **state)
{
  const dt_masks_panel_state_t s_on =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, FALSE, TRUE, FALSE, FALSE);
  assert_true(s_on.want_hosted);
  assert_true(s_on.panel_collapsed);
  assert_true(s_on.corner_icon_visible);
  assert_true(s_on.corner_icon_active);

  const dt_masks_panel_state_t s_off =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE);
  assert_true(s_off.want_hosted);
  assert_true(s_off.panel_collapsed);
  assert_true(s_off.corner_icon_visible);
  assert_false(s_off.corner_icon_active);
}

// when an expanded module is focused and masking-capable:
// - the panel is expanded (unless the user explicitly collapsed it via pref)
// - if expanded, the corner icon is not visible
static void test_panel_state_expanded_module_respects_pref(void **state)
{
  const dt_masks_panel_state_t s_exp =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_true(s_exp.want_hosted);
  assert_false(s_exp.panel_collapsed);
  assert_false(s_exp.corner_icon_visible);

  const dt_masks_panel_state_t s_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_true(s_col.want_hosted);
  assert_true(s_col.panel_collapsed);
  assert_true(s_col.corner_icon_visible);
}

// switching the mask off must not move the panel: its controls stay live with
// the mask off (touching one switches it back on), so the fold follows the
// user's preference exactly as it does with the mask on. Only the corner icon's
// active look reports the mask state.
static void test_panel_state_mask_disabled_does_not_collapse(void **state)
{
  const dt_masks_panel_state_t s =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, FALSE, FALSE, FALSE);
  assert_true(s.want_hosted);
  assert_false(s.panel_collapsed);
  assert_false(s.corner_icon_visible);
  assert_false(s.corner_icon_active);

  // and with the panel folded by preference, the icon is back, still inactive
  const dt_masks_panel_state_t s_folded =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, FALSE, TRUE, FALSE);
  assert_true(s_folded.panel_collapsed);
  assert_true(s_folded.corner_icon_visible);
  assert_false(s_folded.corner_icon_active);
}

// when a module has no masking support (e.g. demosaic, crop) or is unfocused:
// - the panel is collapsed
// - the corner icon is NOT visible
static void test_panel_state_unsupported_or_unfocused_hides_all(void **state)
{
  const dt_masks_panel_state_t s_no_mask =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, FALSE, TRUE, FALSE, FALSE, FALSE);
  assert_false(s_no_mask.want_hosted);
  assert_true(s_no_mask.panel_collapsed);
  assert_false(s_no_mask.corner_icon_visible);

  const dt_masks_panel_state_t s_unfocused =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, FALSE, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_false(s_unfocused.want_hosted);
  assert_true(s_unfocused.panel_collapsed);
  assert_false(s_unfocused.corner_icon_visible);
}

// pinning the mask panel when the module is collapsed must expand the module
static void test_pinning_collapsed_module_expands_iop(void **state)
{
  assert_true(dt_masks_model_pin_should_expand_iop(FALSE, TRUE));
  assert_false(dt_masks_model_pin_should_expand_iop(TRUE, TRUE));
  assert_false(dt_masks_model_pin_should_expand_iop(FALSE, FALSE));
}

// in utility position, panel state follows user preference and does not collapse when IOP is collapsed
static void test_panel_state_utility_position_follows_pref(void **state)
{
  // focused & expanded module in utility position: open when pref is open
  const dt_masks_panel_state_t s_open =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_true(s_open.want_hosted);
  assert_false(s_open.panel_collapsed);
  assert_false(s_open.corner_icon_visible);

  // focused & collapsed IOP module in utility position: stays open when pref is open
  const dt_masks_panel_state_t s_iop_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, FALSE, TRUE, FALSE, FALSE);
  assert_true(s_iop_col.want_hosted);
  assert_false(s_iop_col.panel_collapsed);
  assert_false(s_iop_col.corner_icon_visible);

  // user collapsed the utility expander: collapsed
  const dt_masks_panel_state_t s_user_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_true(s_user_col.want_hosted);
  assert_true(s_user_col.panel_collapsed);
  assert_false(s_user_col.corner_icon_visible);
}

// separate grid panel corner icon and left/right hosting are never active for utility or embedded
static void test_panel_state_no_separate_panel_for_utility_or_embedded(void **state)
{
  const dt_masks_panel_state_t s_util =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_false(s_util.corner_icon_visible);

  const dt_masks_panel_state_t s_emb =
    dt_masks_model_panel_state(MASKS_PANEL_POS_EMBEDDED, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_false(s_emb.want_hosted);
  assert_false(s_emb.corner_icon_visible);
}

// an always-on panel is on screen whatever is focused, collapsed or maskless, and
// only the user's fold takes it away. What it hosts is still the focused
// module's, so only a focused, masking-capable one is hosted
static void test_panel_state_always_on_follows_the_fold_alone(void **state)
{
  for(int focused = 0; focused <= 1; focused++)
    for(int masking = 0; masking <= 1; masking++)
      for(int expanded = 0; expanded <= 1; expanded++)
        for(int active = 0; active <= 1; active++)
        {
          const dt_masks_panel_state_t open =
            dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, focused, masking, expanded,
                                       active, FALSE, TRUE);
          assert_false(open.panel_collapsed);
          assert_false(open.corner_icon_visible);
          assert_int_equal(open.want_hosted, focused && masking);
          assert_int_equal(open.corner_icon_active, focused && masking && active);

          const dt_masks_panel_state_t folded =
            dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, focused, masking, expanded,
                                       active, TRUE, TRUE);
          assert_true(folded.panel_collapsed);
          assert_true(folded.corner_icon_visible);
          assert_int_equal(folded.want_hosted, focused && masking);
        }
}

// and the other two positions do not have the choice
static void test_panel_state_always_on_is_the_canvas_positions_alone(void **state)
{
  for(int pos = MASKS_PANEL_POS_EMBEDDED; pos <= MASKS_PANEL_POS_UTILITY; pos++)
    for(int pref = 0; pref <= 1; pref++)
    {
      const dt_masks_panel_state_t plain =
        dt_masks_model_panel_state(pos, TRUE, TRUE, FALSE, TRUE, pref, FALSE);
      const dt_masks_panel_state_t always_on =
        dt_masks_model_panel_state(pos, TRUE, TRUE, FALSE, TRUE, pref, TRUE);
      assert_int_equal(plain.want_hosted, always_on.want_hosted);
      assert_int_equal(plain.panel_collapsed, always_on.panel_collapsed);
      assert_int_equal(plain.corner_icon_visible, always_on.corner_icon_visible);
    }
}

// the canvas position and the preference together
static void test_panel_always_on_needs_the_canvas_position(void **state)
{
  dt_conf_set_bool("plugins/darkroom/masks/always_on_panel", TRUE);
  dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", MASKS_PANEL_POS_CANVAS);
  assert_true(dt_masks_gui_panel_always_on());

  dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", MASKS_PANEL_POS_UTILITY);
  assert_false(dt_masks_gui_panel_always_on());
  dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", MASKS_PANEL_POS_EMBEDDED);
  assert_false(dt_masks_gui_panel_always_on());

  dt_conf_set_int("plugins/darkroom/blend/masks_panel_position", MASKS_PANEL_POS_CANVAS);
  dt_conf_set_bool("plugins/darkroom/masks/always_on_panel", FALSE);
  assert_false(dt_masks_gui_panel_always_on());
}

// the canvas panel as a floating overlay is not tied to its side panel or to
// the view: only the fold takes it away, whichever way the panel got there
static void test_canvas_panel_floating_ignores_the_side_panel(void **state)
{
  for(int user = 0; user <= 1; user++)
    for(int in_view = 0; in_view <= 1; in_view++)
      for(int side = 0; side <= 1; side++)
        for(int keep = 0; keep <= 1; keep++)
        {
          assert_false(dt_masks_model_canvas_panel_hidden(FALSE, user, FALSE, in_view, side, keep));
          assert_true(dt_masks_model_canvas_panel_hidden(TRUE, user, FALSE, in_view, side, keep));
        }
}

// an always-on panel is part of the side panel it is docked next to, and of the
// darkroom; the fold, or the user opening it, overrides
static void test_canvas_panel_always_on_follows_its_side_panel(void **state)
{
  assert_false(dt_masks_model_canvas_panel_hidden(FALSE, FALSE, TRUE, TRUE, TRUE, FALSE));
  assert_true(dt_masks_model_canvas_panel_hidden(FALSE, FALSE, TRUE, TRUE, FALSE, FALSE));
  assert_true(dt_masks_model_canvas_panel_hidden(FALSE, FALSE, TRUE, FALSE, TRUE, FALSE));
  // opened from its strip beside a hidden side panel
  assert_false(dt_masks_model_canvas_panel_hidden(FALSE, TRUE, TRUE, TRUE, FALSE, FALSE));
  // the fold wins over everything
  for(int user = 0; user <= 1; user++)
    for(int side = 0; side <= 1; side++)
      assert_true(dt_masks_model_canvas_panel_hidden(TRUE, user, TRUE, TRUE, side, TRUE));
}

// pressing the collapse-all key twice is a round trip: the panel is hidden
// with every other panel, then comes back as it was, whether its side panel was
// shown, or hidden with the panel opened from its strip, or hidden with the
// panel hidden along with it
static void test_tab_twice_restores_the_panel(void **state)
{
  for(int always_on = 0; always_on <= 1; always_on++)
    for(int folded = 0; folded <= 1; folded++)
      for(int side_before = 0; side_before <= 1; side_before++)
        for(int opened = 0; opened <= 1; opened++)
        {
          // opening from the strip while its side is shown is no different from
          // the side showing it; only an open panel beside a hidden side is new
          const gboolean user = opened && !side_before;
          const gboolean hidden_before =
            dt_masks_model_canvas_panel_hidden(folded, user, always_on, TRUE, side_before, FALSE);

          // first press: every panel away, the side panel included
          const gboolean hidden_during =
            dt_masks_model_canvas_panel_hidden(folded, FALSE, always_on, TRUE, FALSE, FALSE);
          assert_true(hidden_during || !always_on);

          // second press: the side panel is back as it was, then the panel is
          // given its state from before the first
          const gboolean keep =
            dt_masks_model_tab_keeps_panel(always_on, TRUE, !hidden_before);
          const gboolean hidden_after =
            dt_masks_model_canvas_panel_hidden(folded, FALSE, always_on, TRUE, side_before, keep);
          assert_int_equal(hidden_after, hidden_before);
        }
}

// nothing to give back out of the darkroom, or to a floating panel
static void test_tab_keeps_only_the_always_on_panel_in_view(void **state)
{
  assert_true(dt_masks_model_tab_keeps_panel(TRUE, TRUE, TRUE));
  assert_false(dt_masks_model_tab_keeps_panel(TRUE, TRUE, FALSE));
  assert_false(dt_masks_model_tab_keeps_panel(TRUE, FALSE, TRUE));
  assert_false(dt_masks_model_tab_keeps_panel(FALSE, TRUE, TRUE));
}

static void test_param_channel_tooltips(void **state)
{
  const int csps[] = { DEVELOP_BLEND_CS_LAB, DEVELOP_BLEND_CS_RGB_DISPLAY, DEVELOP_BLEND_CS_RGB_SCENE };
  for(size_t i = 0; i < sizeof(csps) / sizeof(csps[0]); i++)
  {
    const dt_iop_gui_blendif_channel_t *channels =
      dt_develop_blendif_channels_for_csp(csps[i]);
    assert_non_null(channels);
    for(const dt_iop_gui_blendif_channel_t *ch = channels; ch->label; ch++)
    {
      assert_non_null(ch->tooltip);
      char expected[128];
      snprintf(expected, sizeof(expected), "add a parametric element selecting by %s (%s)",
               ch->name, ch->label);
      assert_string_equal(ch->tooltip, expected);
    }
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(test_untouched_parametric_is_a_noop, _teardown),
    cmocka_unit_test_teardown(test_narrowed_parametric_is_not_a_noop, _teardown),
    cmocka_unit_test_teardown(test_output_range_alone_is_not_a_noop, _teardown),
    cmocka_unit_test_teardown(test_inverted_parametric_is_never_a_noop, _teardown),
    cmocka_unit_test_teardown(test_negative_polarity_is_never_a_noop, _teardown),
    cmocka_unit_test_teardown(test_a_shape_is_never_a_noop, _teardown),
    cmocka_unit_test_teardown(test_chosen_view_decides_the_sliders, _teardown),
    cmocka_unit_test_teardown(test_boost_shows_only_with_the_sliders, _teardown),
    cmocka_unit_test_teardown(test_automatic_row_follows_the_output_range, _teardown),
    cmocka_unit_test_teardown(test_view_resolve, _teardown),
    cmocka_unit_test_teardown(test_swatch_span, _teardown),
    cmocka_unit_test_teardown(test_swatch_alpha, _teardown),
    cmocka_unit_test_teardown(test_view_and_boost_share_in_out, _teardown),
    cmocka_unit_test_teardown(test_channel_used_detects_a_touched_range, _teardown),
    cmocka_unit_test_teardown(test_channel_used_honors_the_active_bit, _teardown),
    cmocka_unit_test_setup_teardown(test_sticky_opacity_preference_roundtrips,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test(test_position_options_follow_the_position),
    cmocka_unit_test(test_handle_implies_focus_row),
    cmocka_unit_test_setup_teardown(test_sections_start_expanded,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_sections_fold_independently,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_section_state_is_one_config_key_each,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_controls_fold_per_member_kind,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_drawing_opens_the_controls_without_saving,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_panel_position_preference_roundtrips,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_panel_side_defaults_to_the_processing_modules_side,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_pinned_panel_side_outranks_the_default,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_teardown(test_panel_state_collapsed_module_shows_corner_icon, _teardown),
    cmocka_unit_test_teardown(test_panel_state_expanded_module_respects_pref, _teardown),
    cmocka_unit_test_teardown(test_panel_state_mask_disabled_does_not_collapse, _teardown),
    cmocka_unit_test_teardown(test_panel_state_unsupported_or_unfocused_hides_all, _teardown),
    cmocka_unit_test_teardown(test_panel_state_utility_position_follows_pref, _teardown),
    cmocka_unit_test_teardown(test_panel_state_no_separate_panel_for_utility_or_embedded, _teardown),
    cmocka_unit_test_teardown(test_panel_state_always_on_follows_the_fold_alone, _teardown),
    cmocka_unit_test_teardown(test_panel_state_always_on_is_the_canvas_positions_alone, _teardown),
    cmocka_unit_test(test_canvas_panel_floating_ignores_the_side_panel),
    cmocka_unit_test(test_canvas_panel_always_on_follows_its_side_panel),
    cmocka_unit_test(test_tab_twice_restores_the_panel),
    cmocka_unit_test(test_tab_keeps_only_the_always_on_panel_in_view),
    cmocka_unit_test_setup_teardown(test_panel_always_on_needs_the_canvas_position,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_teardown(test_pinning_collapsed_module_expands_iop, _teardown),
    cmocka_unit_test_teardown(test_param_channel_tooltips, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
