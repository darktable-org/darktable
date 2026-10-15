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

// an expanded row always shows both sub-ranges, whatever the user has touched
static void test_expanded_row_shows_both_ranges(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(TRUE, FALSE, FALSE, FALSE, FALSE);
  assert_true(v.input);
  assert_true(v.output);
}

// the boost-factor slider only exists for channels that have one
static void test_boost_slider_follows_the_channel(void **state)
{
  assert_true(dt_masks_model_param_row_visibility(TRUE, TRUE, TRUE, TRUE, FALSE).boost);
  assert_false(dt_masks_model_param_row_visibility(TRUE, TRUE, TRUE, FALSE, FALSE).boost);
  // never on a collapsed row, whatever the channel supports
  assert_false(dt_masks_model_param_row_visibility(FALSE, TRUE, TRUE, TRUE, FALSE).boost);
}

// a collapsed row adapts: an untouched channel shows only the input slider,
// rather than a second slider that says nothing
static void test_collapsed_untouched_row_shows_input_only(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(FALSE, FALSE, FALSE, FALSE, FALSE);
  assert_true(v.input);
  assert_false(v.output);
}

static void test_collapsed_row_with_only_output_used(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(FALSE, FALSE, TRUE, FALSE, FALSE);
  assert_false(v.input);
  assert_true(v.output);
}

static void test_collapsed_row_with_both_used_shows_both(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(FALSE, TRUE, TRUE, FALSE, FALSE);
  assert_true(v.input);
  assert_true(v.output);
}

static void test_collapsed_row_with_only_input_used(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(FALSE, TRUE, FALSE, FALSE, FALSE);
  assert_true(v.input);
  assert_false(v.output);
}

// the per-sub-range bypass toggles only mean something when both are in play
static void test_bypass_shown_only_when_both_ranges_used(void **state)
{
  assert_true(dt_masks_model_param_row_visibility(FALSE, TRUE, TRUE, FALSE, FALSE).bypass);
  assert_false(dt_masks_model_param_row_visibility(FALSE, TRUE, FALSE, FALSE, FALSE).bypass);
  assert_false(dt_masks_model_param_row_visibility(FALSE, FALSE, TRUE, FALSE, FALSE).bypass);
  assert_false(dt_masks_model_param_row_visibility(TRUE, TRUE, FALSE, FALSE, FALSE).bypass);
}

// a parametric row is an element row like any other: its expanded controls
// lead with a full opacity slider. Its in/out chevron *is* its expander, so the
// slider appears exactly when that row is expanded -- never on a collapsed row.
static void test_parametric_opacity_slider_needs_expanded(void **state)
{
  assert_true(dt_masks_model_param_row_visibility(TRUE, FALSE, FALSE, FALSE, FALSE).opacity);
  assert_false(dt_masks_model_param_row_visibility(FALSE, FALSE, FALSE, FALSE, FALSE).opacity);
}

// and it does not depend on anything about the channel itself -- which
// sub-ranges are used, or whether the channel has a boost factor, decide which
// *channel* sliders show, never whether opacity does
static void test_parametric_opacity_slider_ignores_the_channel_state(void **state)
{
  for(int in_used = 0; in_used < 2; in_used++)
    for(int out_used = 0; out_used < 2; out_used++)
      for(int boost = 0; boost < 2; boost++)
        assert_true(dt_masks_model_param_row_visibility(TRUE, in_used, out_used, boost, FALSE).opacity);
}

// "element properties in subpanel" moves the boost factor and that opacity
// slider out of the row and into its own section; the channel's input and
// output sliders stay in the row
static void test_subpanel_takes_boost_and_opacity_out_of_the_row(void **state)
{
  const dt_masks_param_vis_t v = dt_masks_model_param_row_visibility(TRUE, TRUE, TRUE, TRUE, TRUE);
  assert_true(v.input);
  assert_true(v.output);
  assert_false(v.boost);
  assert_false(v.opacity);
  // collapsed, the row shows what it always does
  const dt_masks_param_vis_t c = dt_masks_model_param_row_visibility(FALSE, TRUE, TRUE, TRUE, TRUE);
  assert_true(c.input);
  assert_true(c.output);
  assert_true(c.bypass);
}

// ---------------------------------------------------------------------------
// what "auto-expand selected" keeps open
// ---------------------------------------------------------------------------

// selection is the anchor whenever there is one, at both levels -- selecting
// an element sets the group half too (see _set_form_target), so picking an
// element anchors its group as well as itself
static void test_auto_expand_anchors_on_the_selection(void **state)
{
  flexi_build("u{1,2,i{3}}");
  flexi_bd.panel_selected_formid = 2;
  flexi_bd.panel_selected_group_cid = 1;
  flexi_bd.masks_last_expanded_elem = 3;
  flexi_bd.masks_last_expanded_group = 3;

  assert_int_equal(dt_masks_model_auto_expand_anchor(&flexi_bd), 2);
  assert_int_equal(dt_masks_model_auto_expand_group_anchor(&flexi_bd), 1);
}

// with nothing selected it falls back to whatever was open last, rather than
// collapsing the panel down to bare headers
static void test_auto_expand_falls_back_to_the_last_expanded(void **state)
{
  flexi_build("u{1,2,i{3}}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = INVALID_MASKID;
  flexi_bd.masks_last_expanded_elem = 2;
  flexi_bd.masks_last_expanded_group = 1;

  assert_int_equal(dt_masks_model_auto_expand_anchor(&flexi_bd), 2);
  assert_int_equal(dt_masks_model_auto_expand_group_anchor(&flexi_bd), 1);
}

// a raster mask expands to its opacity slider, so selecting one makes it the
// anchor like any other element. Its *group* anchors on its own, never
// consulting the element's kind.
static void test_auto_expand_anchors_on_a_raster_mask(void **state)
{
  flexi_build("u{1,2}");
  dt_masks_form_t *f = dt_masks_get_from_id(&flexi_dev, 2);
  const dt_masks_type_t saved = f->type;
  f->type = DT_MASKS_RASTER;

  flexi_bd.panel_selected_formid = 2;
  flexi_bd.panel_selected_group_cid = 1;
  flexi_bd.masks_last_expanded_elem = 1;
  flexi_bd.masks_last_expanded_group = INVALID_MASKID;

  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", TRUE);
  assert_int_equal(dt_masks_model_auto_expand_anchor(&flexi_bd), 2);

  // the group half never consults the element's kind
  assert_int_equal(dt_masks_model_auto_expand_group_anchor(&flexi_bd), 1);

  f->type = saved;
}

// with the properties subpanel a shape row has nothing to expand either, so
// selecting it leaves the open element open too
static void test_auto_expand_ignores_a_shape_with_the_subpanel(void **state)
{
  flexi_build("u{1,2}");
  flexi_bd.panel_selected_formid = 2;
  flexi_bd.panel_selected_group_cid = 1;
  flexi_bd.masks_last_expanded_elem = 1;

  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", TRUE);
  dt_conf_set_bool("plugins/darkroom/masks/properties_subpanel", FALSE);
  assert_int_equal(dt_masks_model_auto_expand_anchor(&flexi_bd), 2);
  dt_conf_set_bool("plugins/darkroom/masks/properties_subpanel", TRUE);
  assert_int_equal(dt_masks_model_auto_expand_anchor(&flexi_bd), 1);
}

// nothing selected and nothing remembered: no anchor, which is what tells the
// panel to fall back to each group's own remembered expanded state rather than
// open exactly one group
static void test_auto_expand_has_no_anchor_when_nothing_is_known(void **state)
{
  flexi_build("u{1,2}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = INVALID_MASKID;
  flexi_bd.masks_last_expanded_elem = INVALID_MASKID;
  flexi_bd.masks_last_expanded_group = INVALID_MASKID;

  assert_false(dt_is_valid_maskid(dt_masks_model_auto_expand_anchor(&flexi_bd)));
  assert_false(dt_is_valid_maskid(dt_masks_model_auto_expand_group_anchor(&flexi_bd)));
}

// the "is this sub-range used" predicate the rule above consumes
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

static void test_auto_expand_preference_roundtrips(void **state)
{
  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", TRUE);
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/auto_expand_selected"));
  dt_conf_set_bool("plugins/darkroom/masks/auto_expand_selected", FALSE);
  assert_false(dt_conf_get_bool("plugins/darkroom/masks/auto_expand_selected"));
}

// the expander option ships with the default the panel's own documentation
// promises: auto-expand on. It comes from darktableconfig.xml via conf_gen.h,
// so an unset key must already read that way -- the panel never writes it
// until the user touches it.
static void test_expander_option_defaults(void **state)
{
  dt_conf_remove_key("plugins/darkroom/masks/auto_expand_selected");
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/auto_expand_selected"));
}

// ---------------------------------------------------------------------------
// which element rows carry an expander chevron
// ---------------------------------------------------------------------------

// every element kind has an expander: its opacity slider at least, which no
// header shows, then a drawn shape's size/hardness/..., and a parametric
// element's in/out sliders (its in/out chevron is its expander)
static void test_every_element_row_is_expandable(void **state)
{
  const dt_masks_type_t kinds[] = { DT_MASKS_CIRCLE,  DT_MASKS_ELLIPSE,
                                    DT_MASKS_PATH,    DT_MASKS_GRADIENT,
                                    DT_MASKS_BRUSH,   DT_MASKS_PARAMETRIC,
                                    DT_MASKS_OBJECT,  DT_MASKS_RASTER };
  for(size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
    assert_true(dt_masks_model_row_is_expandable(kinds[i], FALSE));
}

// forms carry their kind alongside DT_MASKS_CLONE/NON_CLONE and friends, so the
// rule must key off the kind bit rather than the whole type word
static void test_expandability_ignores_the_non_kind_type_bits(void **state)
{
  assert_true(dt_masks_model_row_is_expandable(DT_MASKS_PARAMETRIC | DT_MASKS_NON_CLONE, TRUE));
  assert_false(dt_masks_model_row_is_expandable(DT_MASKS_CIRCLE | DT_MASKS_CLONE, TRUE));
}

// "element properties in subpanel" moves every element's properties to their
// own section, leaving only a parametric row's in/out chevron, which shows the
// channel's input and output sliders
static void test_subpanel_leaves_only_parametric_chevrons(void **state)
{
  const dt_masks_type_t kinds[] = { DT_MASKS_CIRCLE,  DT_MASKS_ELLIPSE, DT_MASKS_PATH,
                                    DT_MASKS_GRADIENT, DT_MASKS_BRUSH,  DT_MASKS_OBJECT,
                                    DT_MASKS_RASTER };
  for(size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
    assert_false(dt_masks_model_row_is_expandable(kinds[i], TRUE));
  assert_true(dt_masks_model_row_is_expandable(DT_MASKS_PARAMETRIC, TRUE));
}

// ---------------------------------------------------------------------------
// section fold states
// ---------------------------------------------------------------------------

// nothing saved yet: every section starts unfolded, from the defaults in
// darktableconfig.xml
static void test_sections_start_expanded(void **state)
{
  for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
  {
    assert_true(dt_masks_model_section_expanded(s, FALSE));
    assert_true(dt_masks_model_section_expanded(s, TRUE));
  }
}

// each section keeps its own state: folding one leaves the others alone
static void test_sections_fold_independently(void **state)
{
  for(dt_masks_section_t folded = 0; folded < DT_MASKS_SECTION_COUNT; folded++)
  {
    dt_masks_model_section_save(folded, FALSE);
    for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
      assert_int_equal(dt_masks_model_section_expanded(s, FALSE), s != folded);
    dt_masks_model_section_save(folded, TRUE);
    for(dt_masks_section_t s = 0; s < DT_MASKS_SECTION_COUNT; s++)
      assert_true(dt_masks_model_section_expanded(s, FALSE));
  }
}

// the state lives in the config, not in any module's panel, so it is the same
// for every module and survives a restart
static void test_section_state_is_one_config_key_each(void **state)
{
  dt_masks_model_section_save(DT_MASKS_SECTION_REFINE, FALSE);
  dt_masks_model_section_save(DT_MASKS_SECTION_PROPS, FALSE);
  dt_masks_model_section_save(DT_MASKS_SECTION_CONSUMERS, FALSE);
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/refinements_collapsed"));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/properties_collapsed"));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/consumers_collapsed"));

  dt_conf_set_bool("plugins/darkroom/masks/consumers_collapsed", FALSE);
  assert_true(dt_masks_model_section_expanded(DT_MASKS_SECTION_CONSUMERS, FALSE));
}

// drawing a shape opens its creation controls in a folded properties
// section, and does not save that: once the drawing ends, the section folds
// back
static void test_drawing_opens_the_properties_without_saving(void **state)
{
  dt_masks_model_section_save(DT_MASKS_SECTION_PROPS, FALSE);
  assert_true(dt_masks_model_section_expanded(DT_MASKS_SECTION_PROPS, TRUE));
  assert_false(dt_masks_model_section_expanded(DT_MASKS_SECTION_PROPS, FALSE));
  assert_true(dt_conf_get_bool("plugins/darkroom/masks/properties_collapsed"));
}

// drawing concerns the properties section only
static void test_drawing_leaves_the_other_sections_folded(void **state)
{
  dt_masks_model_section_save(DT_MASKS_SECTION_REFINE, FALSE);
  dt_masks_model_section_save(DT_MASKS_SECTION_CONSUMERS, FALSE);
  assert_false(dt_masks_model_section_expanded(DT_MASKS_SECTION_REFINE, TRUE));
  assert_false(dt_masks_model_section_expanded(DT_MASKS_SECTION_CONSUMERS, TRUE));
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
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, FALSE, TRUE, FALSE);
  assert_true(s_on.want_hosted);
  assert_true(s_on.panel_collapsed);
  assert_true(s_on.corner_icon_visible);
  assert_true(s_on.corner_icon_active);

  const dt_masks_panel_state_t s_off =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, FALSE, FALSE, FALSE);
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
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_true(s_exp.want_hosted);
  assert_false(s_exp.panel_collapsed);
  assert_false(s_exp.corner_icon_visible);

  const dt_masks_panel_state_t s_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, TRUE, TRUE);
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
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, FALSE, FALSE);
  assert_true(s.want_hosted);
  assert_false(s.panel_collapsed);
  assert_false(s.corner_icon_visible);
  assert_false(s.corner_icon_active);

  // and with the panel folded by preference, the icon is back, still inactive
  const dt_masks_panel_state_t s_folded =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, TRUE, TRUE, FALSE, TRUE);
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
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, TRUE, FALSE, TRUE, FALSE, FALSE);
  assert_false(s_no_mask.want_hosted);
  assert_true(s_no_mask.panel_collapsed);
  assert_false(s_no_mask.corner_icon_visible);

  const dt_masks_panel_state_t s_unfocused =
    dt_masks_model_panel_state(MASKS_PANEL_POS_CANVAS, FALSE, TRUE, TRUE, TRUE, FALSE);
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
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_true(s_open.want_hosted);
  assert_false(s_open.panel_collapsed);
  assert_false(s_open.corner_icon_visible);

  // focused & collapsed IOP module in utility position: stays open when pref is open
  const dt_masks_panel_state_t s_iop_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, FALSE, TRUE, FALSE);
  assert_true(s_iop_col.want_hosted);
  assert_false(s_iop_col.panel_collapsed);
  assert_false(s_iop_col.corner_icon_visible);

  // user collapsed the utility expander: collapsed
  const dt_masks_panel_state_t s_user_col =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, TRUE);
  assert_true(s_user_col.want_hosted);
  assert_true(s_user_col.panel_collapsed);
  assert_false(s_user_col.corner_icon_visible);
}

// separate grid panel corner icon and left/right hosting are never active for utility or embedded
static void test_panel_state_no_separate_panel_for_utility_or_embedded(void **state)
{
  const dt_masks_panel_state_t s_util =
    dt_masks_model_panel_state(MASKS_PANEL_POS_UTILITY, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_false(s_util.corner_icon_visible);

  const dt_masks_panel_state_t s_emb =
    dt_masks_model_panel_state(MASKS_PANEL_POS_EMBEDDED, TRUE, TRUE, TRUE, TRUE, FALSE);
  assert_false(s_emb.want_hosted);
  assert_false(s_emb.corner_icon_visible);
}

// dedicated panel caption reflects module name and instance name with 2-line markup
static void test_masks_panel_header_markup(void **state)
{
  char *m_hosted_no_inst = dt_masks_model_panel_header_markup("exposure", "", TRUE);
  assert_non_null(strstr(m_hosted_no_inst, "blend mask"));
  assert_non_null(strstr(m_hosted_no_inst, "exposure"));
  assert_non_null(strstr(m_hosted_no_inst, "\n"));
  assert_null(strstr(m_hosted_no_inst, "•"));
  free(m_hosted_no_inst);

  char *m_hosted_inst = dt_masks_model_panel_header_markup("exposure", "foreground", TRUE);
  assert_non_null(strstr(m_hosted_inst, "blend mask"));
  assert_non_null(strstr(m_hosted_inst, "exposure"));
  assert_non_null(strstr(m_hosted_inst, "• foreground"));
  assert_non_null(strstr(m_hosted_inst, "\n"));
  free(m_hosted_inst);

  char *m_hosted_no_mod = dt_masks_model_panel_header_markup(NULL, NULL, TRUE);
  assert_non_null(strstr(m_hosted_no_mod, "blend mask"));
  assert_non_null(strstr(m_hosted_no_mod, "no focused module"));
  assert_non_null(strstr(m_hosted_no_mod, "\n"));
  free(m_hosted_no_mod);

  char *m_embedded = dt_masks_model_panel_header_markup("exposure", "foreground", FALSE);
  assert_string_equal(m_embedded, "blend mask");
  free(m_embedded);
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
    cmocka_unit_test_teardown(test_expanded_row_shows_both_ranges, _teardown),
    cmocka_unit_test_teardown(test_boost_slider_follows_the_channel, _teardown),
    cmocka_unit_test_teardown(test_collapsed_untouched_row_shows_input_only, _teardown),
    cmocka_unit_test_teardown(test_collapsed_row_with_only_output_used, _teardown),
    cmocka_unit_test_teardown(test_collapsed_row_with_both_used_shows_both, _teardown),
    cmocka_unit_test_teardown(test_collapsed_row_with_only_input_used, _teardown),
    cmocka_unit_test_teardown(test_bypass_shown_only_when_both_ranges_used, _teardown),
    cmocka_unit_test_teardown(test_parametric_opacity_slider_ignores_the_channel_state,
                              _teardown),
    cmocka_unit_test_teardown(test_subpanel_takes_boost_and_opacity_out_of_the_row,
                              _teardown),
    cmocka_unit_test_teardown(test_parametric_opacity_slider_needs_expanded,
                              _teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_anchors_on_the_selection,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_falls_back_to_the_last_expanded,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_anchors_on_a_raster_mask,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_ignores_a_shape_with_the_subpanel,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_has_no_anchor_when_nothing_is_known,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_teardown(test_channel_used_detects_a_touched_range, _teardown),
    cmocka_unit_test_teardown(test_channel_used_honors_the_active_bit, _teardown),
    cmocka_unit_test_setup_teardown(test_sticky_opacity_preference_roundtrips,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_auto_expand_preference_roundtrips,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_expander_option_defaults,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_teardown(test_every_element_row_is_expandable, _teardown),
    cmocka_unit_test_teardown(test_expandability_ignores_the_non_kind_type_bits,
                              _teardown),
    cmocka_unit_test_teardown(test_subpanel_leaves_only_parametric_chevrons, _teardown),
    cmocka_unit_test_setup_teardown(test_sections_start_expanded,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_sections_fold_independently,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_section_state_is_one_config_key_each,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_drawing_opens_the_properties_without_saving,
                                    _conf_setup, _conf_teardown),
    cmocka_unit_test_setup_teardown(test_drawing_leaves_the_other_sections_folded,
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
    cmocka_unit_test_teardown(test_pinning_collapsed_module_expands_iop, _teardown),
    cmocka_unit_test_teardown(test_masks_panel_header_markup, _teardown),
    cmocka_unit_test_teardown(test_param_channel_tooltips, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
