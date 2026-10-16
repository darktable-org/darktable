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

// The panel's own controls, as opposed to the mask they edit: where the
// precise-entry popup of a parametric slider is placed, and how that popup's
// numbers map to and from the [0,1] fractions the slider nodes actually store.
//
// Both are pure functions over values, and both are the kind of code that
// looks right on the machine it was written on and misbehaves everywhere else
// -- a popup placed in the wrong coordinate space lands on the primary monitor
// for anyone whose darktable is not on it, and a hue range that comes out a
// hair off 360 silently loses the color wheel. Neither shows up without a
// second display or a specific channel selected, which is exactly why they are
// worth pinning here.

#include "flexi_fixture.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

// ---------------------------------------------------------------------------
// precise-entry popup placement
// ---------------------------------------------------------------------------

// a plain single-monitor setup: a 1920x1080 work area, the mask panel down the
// right-hand side, and a slider row 400px down it. Individual tests move the
// one thing they are about.
static dt_masks_whisker_geom_t _geom(void)
{
  const dt_masks_whisker_geom_t g = {
    .anchor = { 1600, 400, 300, 20 },
    .center_x = 1750,
    .workarea = { 0, 0, 1920, 1080 },
    .panel_x = 1600,
    .panel_w = 320,
    .size = 180,
    .gap = 6,
  };
  return g;
}

// the popup goes below the slider when there is room, leaving the gap
static void test_popup_sits_below_when_there_is_room(void **state)
{
  const dt_masks_whisker_geom_t g = _geom();
  const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);

  assert_int_equal(r.y, 400 + 20 + 6);
  assert_int_equal(r.width, 180);
  assert_int_equal(r.height, 180);
}

// ...and flips above once the bottom of the screen is too close for it
static void test_popup_flips_above_when_below_is_short(void **state)
{
  dt_masks_whisker_geom_t g = _geom();
  g.anchor.y = 1000; // only 60px of work area left below the slider

  const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);
  assert_int_equal(r.y, 1000 - 6 - 180);
}

// the whole point of placing it off to one side: the popup drives the control
// points on the slider, so it must never cover the slider it belongs to. This
// has to hold wherever the slider is, including the two edges where the
// placement rule changes its mind.
static void test_popup_never_overlaps_the_slider(void **state)
{
  for(gint anchor_y = 0; anchor_y <= 1060; anchor_y += 10)
  {
    dt_masks_whisker_geom_t g = _geom();
    g.anchor.y = anchor_y;
    const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);

    const gboolean disjoint =
      (r.y + r.height <= g.anchor.y) || (r.y >= g.anchor.y + g.anchor.height);
    if(!disjoint)
      fail_msg("popup %d..%d overlaps slider %d..%d", r.y, r.y + r.height,
               g.anchor.y, g.anchor.y + g.anchor.height);
  }
}

// a popup that hangs off the work area gets squashed to fit by GDK rather than
// moved (GDK_ANCHOR_RESIZE_Y), which would leave the hue wheel an ellipse -- so
// staying on screen wins over clearing the slider when a short screen makes the
// two impossible together
static void test_popup_stays_on_screen_when_neither_side_fits(void **state)
{
  dt_masks_whisker_geom_t g = _geom();
  g.workarea.height = 220; // no room for a 180px popup either side of the row
  g.anchor.y = 100;

  const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);
  assert_true(r.y >= g.workarea.y);
  assert_true(r.y + r.height <= g.workarea.y + g.workarea.height);
}

// centred on the node that was right-clicked, not on the row: the popup opens
// over the value it edits, the way a bauhaus slider's own popup does
static void test_popup_centres_on_the_requested_point(void **state)
{
  dt_masks_whisker_geom_t g = _geom();
  g.center_x = 1750;
  assert_int_equal(dt_masks_model_whisker_popup_rect(&g).x, 1750 - 90);
}

// but never at the cost of leaving the panel: a node near either end of the
// slider would otherwise push the popup out over the image, covering the very
// thing the user is judging the edit against
static void test_popup_stays_within_the_panel(void **state)
{
  const dt_masks_whisker_geom_t base = _geom();

  for(gint center = base.panel_x; center <= base.panel_x + base.panel_w; center += 5)
  {
    dt_masks_whisker_geom_t g = base;
    g.center_x = center;
    const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);

    if(r.x < g.panel_x || r.x + r.width > g.panel_x + g.panel_w)
      fail_msg("popup %d..%d escapes panel %d..%d (centre %d)", r.x, r.x + r.width,
               g.panel_x, g.panel_x + g.panel_w, center);
  }
}

// the placement is in root coordinates throughout, so a second monitor is just
// a large x offset: the popup must follow the panel onto it, not stay on the
// primary display, as window-relative arithmetic would make it
static void test_popup_follows_the_panel_onto_a_second_monitor(void **state)
{
  dt_masks_whisker_geom_t g = _geom();
  g.workarea = (GdkRectangle){ 1920, 0, 1920, 1080 };
  g.anchor.x += 1920;
  g.center_x += 1920;
  g.panel_x += 1920;

  const GdkRectangle r = dt_masks_model_whisker_popup_rect(&g);
  assert_true(r.x >= 1920);
  assert_int_equal(r.x, 1750 + 1920 - 90);
}

// ---------------------------------------------------------------------------
// what the popup's numbers mean
// ---------------------------------------------------------------------------

// a channel of each of the three display kinds. Lab has all three: L prints as
// a percentage, a/b print as signed Lab coordinates, h prints as an angle.
static const dt_iop_gui_blendif_channel_t *_lab_channel(const int idx)
{
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_LAB);
  assert_non_null(ch);
  return &ch[idx];
}

#define LAB_L 0
#define LAB_a 1
#define LAB_h 4

// bauhaus gives a slider the color wheel -- and the wrap-around past either end
// that an angle needs -- only when its span is *exactly* 360 with a "°" format
// (see _is_full_circle in bauhaus.c). Nothing warns when it is not: the popup
// just quietly opens as a plain bar. This is the assertion that keeps the hue
// popup a wheel.
static void test_hue_spans_exactly_360_degrees(void **state)
{
  const dt_iop_gui_blendif_channel_t *h = _lab_channel(LAB_h);

  const float lo = dt_masks_gui_param_row_slider_precise_display(h, 1.0f, 0.0f);
  const float hi = dt_masks_gui_param_row_slider_precise_display(h, 1.0f, 1.0f);

  // exact, not approximate: bauhaus tests the span with == against 360.0f, so
  // a value that is merely very close is a value that loses the wheel
  assert_true(lo == 0.0f);
  assert_true(hi == 360.0f);
  assert_true(hi - lo == 360.0f);
}

// the middle of the hue slider is 180 degrees, not 0.5 of something
static void test_hue_reads_as_degrees(void **state)
{
  const dt_iop_gui_blendif_channel_t *h = _lab_channel(LAB_h);
  assert_float_equal(dt_masks_gui_param_row_slider_precise_display(h, 1.0f, 0.5f), 180.0f, 1e-4f);
}

// a hue's displayed value must not depend on the boost factor: boost scales a
// channel's magnitude, and an angle has none. Getting this wrong would make the
// span drift off 360 and cost the wheel.
static void test_hue_ignores_the_boost_factor(void **state)
{
  const dt_iop_gui_blendif_channel_t *h = _lab_channel(LAB_h);

  const float boosts[] = { 0.25f, 1.0f, 4.0f };
  for(size_t i = 0; i < sizeof(boosts) / sizeof(*boosts); i++)
  {
    assert_true(dt_masks_gui_param_row_slider_precise_display(h, boosts[i], 1.0f) == 360.0f);
    assert_float_equal(dt_masks_gui_param_row_slider_precise_display(h, boosts[i], 0.25f), 90.0f,
                       1e-4f);
  }
}

// typing a value into the popup goes through parse(); dragging a node updates
// the popup through display(). The two are the same conversion in opposite
// directions, so a value that survives one and then the other has to come back
// unchanged -- otherwise a node creeps every time the popup is opened on it.
static void test_display_and_parse_round_trip(void **state)
{
  const int channels[] = { LAB_L, LAB_a, LAB_h };
  const float boosts[] = { 0.5f, 1.0f, 2.0f };
  const float fracs[] = { 0.0f, 0.125f, 0.5f, 0.75f, 1.0f };

  for(size_t c = 0; c < sizeof(channels) / sizeof(*channels); c++)
  {
    const dt_iop_gui_blendif_channel_t *ch = _lab_channel(channels[c]);
    for(size_t b = 0; b < sizeof(boosts) / sizeof(*boosts); b++)
    {
      for(size_t f = 0; f < sizeof(fracs) / sizeof(*fracs); f++)
      {
        const float shown =
          dt_masks_gui_param_row_slider_precise_display(ch, boosts[b], fracs[f]);
        const float back = dt_masks_gui_param_row_slider_precise_parse(ch, boosts[b], shown);
        if(fabsf(back - fracs[f]) > 1e-5f)
          fail_msg("channel %d boost %.2f: %.4f -> %.4f -> %.6f", channels[c],
                   boosts[b], fracs[f], shown, back);
      }
    }
  }
}

// the a/b channels are signed and centred on zero, unlike every other channel,
// which is why they get their own formula rather than the default percentage
static void test_lab_ab_is_signed_around_zero(void **state)
{
  const dt_iop_gui_blendif_channel_t *a = _lab_channel(LAB_a);

  assert_float_equal(dt_masks_gui_param_row_slider_precise_display(a, 1.0f, 0.5f), 0.0f, 1e-4f);
  assert_true(dt_masks_gui_param_row_slider_precise_display(a, 1.0f, 0.0f) < 0.0f);
  assert_true(dt_masks_gui_param_row_slider_precise_display(a, 1.0f, 1.0f) > 0.0f);
}

// ...and unlike hue, a/b and the percentage channels *do* scale with boost:
// that is what boost is for, and a popup that ignored it would put a typed
// value at the wrong place on the slider
static void test_magnitude_channels_scale_with_boost(void **state)
{
  const dt_iop_gui_blendif_channel_t *l = _lab_channel(LAB_L);

  const float at_1 = dt_masks_gui_param_row_slider_precise_display(l, 1.0f, 1.0f);
  const float at_2 = dt_masks_gui_param_row_slider_precise_display(l, 2.0f, 1.0f);
  assert_float_equal(at_2, at_1 * 2.0f, 1e-3f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_popup_sits_below_when_there_is_room),
    cmocka_unit_test(test_popup_flips_above_when_below_is_short),
    cmocka_unit_test(test_popup_never_overlaps_the_slider),
    cmocka_unit_test(test_popup_stays_on_screen_when_neither_side_fits),
    cmocka_unit_test(test_popup_centres_on_the_requested_point),
    cmocka_unit_test(test_popup_stays_within_the_panel),
    cmocka_unit_test(test_popup_follows_the_panel_onto_a_second_monitor),
    cmocka_unit_test(test_hue_spans_exactly_360_degrees),
    cmocka_unit_test(test_hue_reads_as_degrees),
    cmocka_unit_test(test_hue_ignores_the_boost_factor),
    cmocka_unit_test(test_display_and_parse_round_trip),
    cmocka_unit_test(test_lab_ab_is_signed_around_zero),
    cmocka_unit_test(test_magnitude_channels_scale_with_boost),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
