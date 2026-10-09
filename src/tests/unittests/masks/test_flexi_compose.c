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

// The mask operators themselves: the arithmetic behind every operator name the
// panel shows.
//
// Rendering an image and comparing pixels shows the pipeline agrees with
// itself on its fixtures, but does not pin what an operator means, nor the
// properties the rest of the design leans on:
//
//   * maximum and screen combine members order-independently, so reordering
//     the members of such a group changes nothing; difference does not, its
//     first member being the base;
//   * an empty group is the identity for its operator -- which is what stops an
//     empty intersection group from blanking the entire mask;
//   * opacity and invert compose the same way for every operator.
//
// Each is asserted here directly, on small buffers with known values, rather
// than being inferred from an image.

#include "flexi_fixture.h"
#include "develop/masks/group_internal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <math.h>

#define N 5
// a spread of values including both endpoints, since the operators clamp and
// saturate differently at 0 and 1
static const float A[N] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
static const float B[N] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };

typedef void (*combine_fn)(float *const restrict, float *const restrict,
                           const size_t, const float, const int);

static void _apply(combine_fn fn, const float *dest_in, const float *src,
                   float *out, const float opacity, const int inverted)
{
  float s[N];
  memcpy(out, dest_in, sizeof(float) * N);
  memcpy(s, src, sizeof(float) * N);
  fn(out, s, N, opacity, inverted);
}

static void _assert_close(const float *got, const float *want, const char *what)
{
  for(int i = 0; i < N; i++)
    if(fabsf(got[i] - want[i]) > 1e-5f)
      fail_msg("%s: element %d is %.6f, expected %.6f", what, i, got[i], want[i]);
}

// ---------------------------------------------------------------------------
// what each operator computes
// ---------------------------------------------------------------------------

static void test_maximum_is_max(void **state)
{
  float out[N];
  _apply(dt_masks_combine_maximum, A, B, out, 1.0f, 0);
  const float want[N] = { 1.0f, 0.75f, 0.5f, 0.75f, 1.0f };
  _assert_close(out, want, "maximum");
}

static void test_minimum_is_min(void **state)
{
  float out[N];
  _apply(dt_masks_combine_minimum, A, B, out, 1.0f, 0);
  const float want[N] = { 0.0f, 0.25f, 0.5f, 0.25f, 0.0f };
  _assert_close(out, want, "minimum");
}

// difference removes the incoming mask from the accumulator
static void test_difference_subtracts(void **state)
{
  float out[N];
  _apply(dt_masks_combine_difference, A, B, out, 1.0f, 0);
  // a * (1 - b): b == 0 keeps a, b == 1 removes it
  const float want[N] = { 0.0f, 0.0625f, 0.25f, 0.5625f, 1.0f };
  _assert_close(out, want, "difference");
}

static void test_screen_is_the_probabilistic_or(void **state)
{
  float out[N];
  _apply(dt_masks_combine_screen, A, B, out, 1.0f, 0);
  float want[N];
  for(int i = 0; i < N; i++) want[i] = A[i] + B[i] - A[i] * B[i];
  _assert_close(out, want, "screen");
}

static void test_product_multiplies(void **state)
{
  float out[N];
  _apply(dt_masks_combine_product, A, B, out, 1.0f, 0);
  float want[N];
  for(int i = 0; i < N; i++) want[i] = A[i] * B[i];
  _assert_close(out, want, "product");
}

// sum and exclusion must stay in range even where their raw arithmetic would
// not -- a + b saturates at 1, and exclusion is symmetric about it
static void test_sum_and_exclusion_stay_in_range(void **state)
{
  float out[N];
  _apply(dt_masks_combine_sum, A, B, out, 1.0f, 0);
  for(int i = 0; i < N; i++)
    if(out[i] < -1e-5f || out[i] > 1.0f + 1e-5f)
      fail_msg("sum left element %d out of range: %.6f", i, out[i]);

  _apply(dt_masks_combine_exclusion, A, B, out, 1.0f, 0);
  for(int i = 0; i < N; i++)
    if(out[i] < -1e-5f || out[i] > 1.0f + 1e-5f)
      fail_msg("exclusion left element %d out of range: %.6f", i, out[i]);
}

// every operator must keep its output in [0,1] for every input in [0,1]:
// a mask outside that range is meaningless to the blend math downstream
static void test_every_operator_keeps_the_mask_in_range(void **state)
{
  const struct { const char *name; combine_fn fn; } ops[] = {
    { "maximum", dt_masks_combine_maximum },
    { "minimum", dt_masks_combine_minimum },
    { "difference", dt_masks_combine_difference },
    { "sum", dt_masks_combine_sum },
    { "exclusion", dt_masks_combine_exclusion },
    { "product", dt_masks_combine_product },
    { "screen", dt_masks_combine_screen },
  };
  const float opacities[] = { 0.0f, 0.35f, 1.0f };

  for(size_t o = 0; o < sizeof(ops) / sizeof(*ops); o++)
    for(size_t k = 0; k < sizeof(opacities) / sizeof(*opacities); k++)
      for(int inv = 0; inv < 2; inv++)
      {
        float out[N];
        _apply(ops[o].fn, A, B, out, opacities[k], inv);
        for(int i = 0; i < N; i++)
          if(!(out[i] >= -1e-5f && out[i] <= 1.0f + 1e-5f) || isnan(out[i]))
            fail_msg("%s (opacity %.2f, inverted %d) produced %.6f at element %d",
                     ops[o].name, opacities[k], inv, out[i], i);
      }
}

// ---------------------------------------------------------------------------
// order independence, and where it does not hold
// ---------------------------------------------------------------------------

// union and screen must be commutative, or reordering the members of such a
// group would silently change the rendered mask
static void test_group_fold_operators_are_commutative(void **state)
{
  float ab[N], ba[N];

  _apply(dt_masks_combine_maximum, A, B, ab, 1.0f, 0);
  _apply(dt_masks_combine_maximum, B, A, ba, 1.0f, 0);
  _assert_close(ab, ba, "union is not commutative");

  _apply(dt_masks_combine_screen, A, B, ab, 1.0f, 0);
  _apply(dt_masks_combine_screen, B, A, ba, 1.0f, 0);
  _assert_close(ab, ba, "screen is not commutative");
}

// and associative, so a three-member group folds to the same mask whatever
// order the members are visited in
static void test_group_fold_operators_are_associative(void **state)
{
  const float C[N] = { 0.6f, 0.1f, 0.9f, 0.4f, 0.2f };
  float ab[N], abc[N], bc[N], a_bc[N];

  _apply(dt_masks_combine_maximum, A, B, ab, 1.0f, 0);
  _apply(dt_masks_combine_maximum, ab, C, abc, 1.0f, 0);
  _apply(dt_masks_combine_maximum, B, C, bc, 1.0f, 0);
  _apply(dt_masks_combine_maximum, A, bc, a_bc, 1.0f, 0);
  _assert_close(abc, a_bc, "union is not associative");

  _apply(dt_masks_combine_screen, A, B, ab, 1.0f, 0);
  _apply(dt_masks_combine_screen, ab, C, abc, 1.0f, 0);
  _apply(dt_masks_combine_screen, B, C, bc, 1.0f, 0);
  _apply(dt_masks_combine_screen, A, bc, a_bc, 1.0f, 0);
  _assert_close(abc, a_bc, "screen is not associative");
}

// difference is deliberately NOT commutative: a group folding by difference
// takes its first member as the base, so its order is the user's choice
static void test_difference_is_order_dependent(void **state)
{
  float ab[N], ba[N];
  _apply(dt_masks_combine_difference, A, B, ab, 1.0f, 0);
  _apply(dt_masks_combine_difference, B, A, ba, 1.0f, 0);

  gboolean same = TRUE;
  for(int i = 0; i < N; i++) if(fabsf(ab[i] - ba[i]) > 1e-5f) same = FALSE;
  if(same) fail_msg("difference came out order-independent; it must not be");
}

// ---------------------------------------------------------------------------
// the nested forms: difference and exclusion expressed with order-free
// operators
// ---------------------------------------------------------------------------

// fold `src` into a group seeded empty, at `opacity`, by screen; then invert
// the finished sub-mask, the way a group's invert-output does
// (_group_get_mask_roi_flexi)
static void _inverted_screen_group(const float *src, const float opacity, float *out)
{
  const float empty[N] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
  _apply(dt_masks_combine_screen, empty, src, out, opacity, 0);
  for(int i = 0; i < N; i++) out[i] = 1.0f - out[i];
}

/* A hole is `product { A, inverted screen group { B } }`. The screen group's
   sub-mask is o*B, inverting it gives 1 - o*B, and multiplying that into A is
   exactly classic's acc * (1 - o*B) (dt_masks_combine_difference).

   The opacity ORDER is the whole point: inside the group the hole's opacity
   applies before the invert, which is what the negative control below pins. */
static void test_a_faded_hole_is_a_product_of_an_inverted_screen_group(void **state)
{
  // the corpus's faded holes sit at 0.36, 0.75 and 0.93; 1.0 must agree too
  const float opacities[] = { 0.36f, 0.75f, 0.93f, 1.0f, 0.0f };
  for(size_t k = 0; k < sizeof(opacities) / sizeof(*opacities); k++)
  {
    float classic[N], sub[N], got[N];
    _apply(dt_masks_combine_difference, A, B, classic, opacities[k], 0);
    _inverted_screen_group(B, opacities[k], sub);
    _apply(dt_masks_combine_product, A, sub, got, 1.0f, 0);
    _assert_close(got, classic, "faded hole is not the inverted screen group");
  }
}

// and a RUN of holes is one screen group: 1 - (1 - o1*x1)(1 - o2*x2) is what
// the screen fold computes, which is classic subtracting each hole in turn
static void test_a_run_of_faded_holes_folds_into_one_screen_group(void **state)
{
  const float C[N] = { 0.6f, 0.1f, 0.9f, 0.4f, 0.2f };
  const float o1 = 0.36f, o2 = 0.75f;

  float classic[N], step[N];
  _apply(dt_masks_combine_difference, A, B, step, o1, 0);
  _apply(dt_masks_combine_difference, step, C, classic, o2, 0);

  const float empty[N] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
  float sub[N], sub2[N], inv[N], got[N];
  _apply(dt_masks_combine_screen, empty, B, sub, o1, 0);
  _apply(dt_masks_combine_screen, sub, C, sub2, o2, 0);
  for(int i = 0; i < N; i++) inv[i] = 1.0f - sub2[i];
  _apply(dt_masks_combine_product, A, inv, got, 1.0f, 0);

  _assert_close(got, classic, "a run of holes is not one screen group");
}

/* The negative control, and the reason the operand is wrapped in a group at
   all: an element's own invert applies its opacity AFTER inverting
   (the `inverted` branch of dt_masks_combine_*), giving o*(1 - B) instead
   of 1 - o*B. At a fractional
   opacity those are different masks, so inverting the element directly would
   silently change every faded hole. */
static void test_inverting_the_element_is_not_the_faded_hole_form(void **state)
{
  const float o = 0.36f;
  float classic[N], elem[N], got[N];
  _apply(dt_masks_combine_difference, A, B, classic, o, 0);
  // an element inverted in place: opacity applied to the complement
  for(int i = 0; i < N; i++) elem[i] = o * (1.0f - B[i]);
  _apply(dt_masks_combine_product, A, elem, got, 1.0f, 0);

  gboolean same = TRUE;
  for(int i = 0; i < N; i++) if(fabsf(got[i] - classic[i]) > 1e-5f) same = FALSE;
  if(same)
    fail_msg("inverting the element matched the hole form; the group wrapper"
             " would then be pointless and the ordering claim is wrong");
}

/* Exclusion is `maximum { product { A, inverted P }, product { P, inverted A } }`.
   Classic computes MAX((1-A)*P, A*(1-P)) where both operands are positive, and
   falls back to MAX(A, P) where either is zero (dt_masks_combine_exclusion): the two
   agree, since with A or P zero the products reduce to the surviving operand. */
static void test_exclusion_is_the_maximum_of_two_products(void **state)
{
  const float opacities[] = { 0.36f, 1.0f, 0.0f };
  for(size_t k = 0; k < sizeof(opacities) / sizeof(*opacities); k++)
  {
    const float o = opacities[k];
    float classic[N], got[N];
    _apply(dt_masks_combine_exclusion, A, B, classic, o, 0);
    for(int i = 0; i < N; i++)
    {
      const float p = o * B[i];
      got[i] = fmaxf(A[i] * (1.0f - p), p * (1.0f - A[i]));
    }
    _assert_close(got, classic, "exclusion is not the maximum of two products");
  }
}

// a group's finished sub-mask: inverted, then scaled by its group opacity
// (group.c _group_get_mask_roi_flexi)
static void _group_output(const float *x, const float go, const int inverted, float *out)
{
  for(int i = 0; i < N; i++) out[i] = go * (inverted ? 1.0f - x[i] : x[i]);
}

/* Migration moves a nested group reference's opacity and inversion onto the
   group's marker (masks.c _fold_nested_refs), so the panel can show them on
   the group's header. Every flexi combine must see the same member:
   o * (inv ? 1 - g : g) with g = go * (minv ? 1 - x : x). Uninverted, the
   opacities multiply; inverted over a group at full opacity, the inversion
   flips and the opacity moves across. */
static void test_a_nested_reference_folds_onto_its_group(void **state)
{
  const combine_fn fns[] = { dt_masks_combine_maximum, dt_masks_combine_minimum,
                             dt_masks_combine_screen, dt_masks_combine_product,
                             dt_masks_combine_sum };
  const float opacities[] = { 0.36f, 0.75f, 1.0f };
  for(size_t f = 0; f < sizeof(fns) / sizeof(*fns); f++)
    for(size_t a = 0; a < sizeof(opacities) / sizeof(*opacities); a++)
      for(size_t b = 0; b < sizeof(opacities) / sizeof(*opacities); b++)
        for(int inv = 0; inv < 2; inv++)
          for(int minv = 0; minv < 2; minv++)
          {
            const float o = opacities[a], go = opacities[b];
            if(inv && go != 1.0f) continue; // not foldable, left as it is
            float g[N], want[N], folded[N], got[N];
            _group_output(B, go, minv, g);
            _apply(fns[f], A, g, want, o, inv);
            _group_output(B, go * o, minv ^ inv, folded);
            _apply(fns[f], A, folded, got, 1.0f, 0);
            _assert_close(got, want, "a folded nested reference renders differently");
          }
}

/* ... and the case it leaves alone: an inverted reference over a faded group
   is o * (1 - go * x), which no single group opacity and inversion produce */
static void test_an_inverted_reference_over_a_faded_group_does_not_fold(void **state)
{
  const float o = 0.75f, go = 0.36f;
  float g[N], want[N], folded[N], got[N];
  _group_output(B, go, 0, g);
  _apply(dt_masks_combine_maximum, A, g, want, o, 1);
  _group_output(B, go * o, 1, folded);
  _apply(dt_masks_combine_maximum, A, folded, got, 1.0f, 0);
  gboolean differs = FALSE;
  for(int i = 0; i < N; i++) differs |= fabsf(got[i] - want[i]) > 1e-5f;
  assert_true(differs);
}

// ---------------------------------------------------------------------------
// identities -- what makes an empty group harmless
// ---------------------------------------------------------------------------

// An empty group contributes nothing, which the compositor implements by
// skipping it. These pin the arithmetic that makes skipping the *right*
// choice: compositing an all-zero mask is already the identity for maximum,
// and an all-one mask is the identity for intersection -- so an empty
// intersection group could never be allowed to composite as all-zero.
static void test_maximum_with_zero_is_identity(void **state)
{
  const float zero[N] = { 0 };
  float out[N];
  _apply(dt_masks_combine_maximum, A, zero, out, 1.0f, 0);
  _assert_close(out, A, "union with an empty mask changed the accumulator");
}

static void test_minimum_with_one_is_identity(void **state)
{
  const float one[N] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
  float out[N];
  _apply(dt_masks_combine_minimum, A, one, out, 1.0f, 0);
  _assert_close(out, A, "intersection with a full mask changed the accumulator");
}

static void test_product_with_one_is_identity(void **state)
{
  const float one[N] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
  float out[N];
  _apply(dt_masks_combine_product, A, one, out, 1.0f, 0);
  _assert_close(out, A, "multiply by a full mask changed the accumulator");
}

static void test_screen_with_zero_is_identity(void **state)
{
  const float zero[N] = { 0 };
  float out[N];
  _apply(dt_masks_combine_screen, A, zero, out, 1.0f, 0);
  _assert_close(out, A, "screen with an empty mask changed the accumulator");
}

static void test_difference_with_zero_is_identity(void **state)
{
  const float zero[N] = { 0 };
  float out[N];
  _apply(dt_masks_combine_difference, A, zero, out, 1.0f, 0);
  _assert_close(out, A, "difference by an empty mask changed the accumulator");
}

// ---------------------------------------------------------------------------
// opacity and invert
// ---------------------------------------------------------------------------

// opacity 0 makes the incoming mask contribute nothing -- the same identity as
// an empty mask, for the operators whose identity is zero
static void test_zero_opacity_neutralizes_a_maximum_member(void **state)
{
  float out[N];
  _apply(dt_masks_combine_maximum, A, B, out, 0.0f, 0);
  _assert_close(out, A, "a zero-opacity union member still changed the mask");
}

// invert complements the incoming mask before the operator sees it, so
// inverting an all-zero mask makes it behave like an all-one one
static void test_invert_complements_the_incoming_mask(void **state)
{
  const float zero[N] = { 0 };
  const float one[N] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
  float inverted_zero[N], plain_one[N];

  _apply(dt_masks_combine_maximum, A, zero, inverted_zero, 1.0f, 1);
  _apply(dt_masks_combine_maximum, A, one, plain_one, 1.0f, 0);
  _assert_close(inverted_zero, plain_one,
                "inverting an empty mask did not behave like a full one");
}

// inverting twice is the identity, for every operator
static void test_double_invert_is_identity(void **state)
{
  const struct { const char *name; combine_fn fn; } ops[] = {
    { "maximum", dt_masks_combine_maximum },
    { "minimum", dt_masks_combine_minimum },
    { "difference", dt_masks_combine_difference },
    { "sum", dt_masks_combine_sum },
    { "exclusion", dt_masks_combine_exclusion },
    { "product", dt_masks_combine_product },
    { "screen", dt_masks_combine_screen },
  };
  float b_inv[N];
  for(int i = 0; i < N; i++) b_inv[i] = 1.0f - B[i];

  for(size_t o = 0; o < sizeof(ops) / sizeof(*ops); o++)
  {
    float via_flag[N], via_data[N];
    _apply(ops[o].fn, A, B, via_flag, 1.0f, 1);      // invert flag on B
    _apply(ops[o].fn, A, b_inv, via_data, 1.0f, 0);  // pre-inverted data
    _assert_close(via_flag, via_data, ops[o].name);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_maximum_is_max),
    cmocka_unit_test(test_minimum_is_min),
    cmocka_unit_test(test_difference_subtracts),
    cmocka_unit_test(test_screen_is_the_probabilistic_or),
    cmocka_unit_test(test_product_multiplies),
    cmocka_unit_test(test_sum_and_exclusion_stay_in_range),
    cmocka_unit_test(test_every_operator_keeps_the_mask_in_range),
    cmocka_unit_test(test_group_fold_operators_are_commutative),
    cmocka_unit_test(test_group_fold_operators_are_associative),
    cmocka_unit_test(test_difference_is_order_dependent),
    cmocka_unit_test(test_a_faded_hole_is_a_product_of_an_inverted_screen_group),
    cmocka_unit_test(test_a_run_of_faded_holes_folds_into_one_screen_group),
    cmocka_unit_test(test_inverting_the_element_is_not_the_faded_hole_form),
    cmocka_unit_test(test_exclusion_is_the_maximum_of_two_products),
    cmocka_unit_test(test_a_nested_reference_folds_onto_its_group),
    cmocka_unit_test(test_an_inverted_reference_over_a_faded_group_does_not_fold),
    cmocka_unit_test(test_maximum_with_zero_is_identity),
    cmocka_unit_test(test_minimum_with_one_is_identity),
    cmocka_unit_test(test_product_with_one_is_identity),
    cmocka_unit_test(test_screen_with_zero_is_identity),
    cmocka_unit_test(test_difference_with_zero_is_identity),
    cmocka_unit_test(test_zero_opacity_neutralizes_a_maximum_member),
    cmocka_unit_test(test_invert_complements_the_incoming_mask),
    cmocka_unit_test(test_double_invert_is_identity),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
