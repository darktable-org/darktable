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
/*
 * cmocka unit tests for the presenter's headroom target
 * (gui/presenter_target.c) on a simulated clock: what the platform reports
 * becomes a target in 1/8 EV steps rounded down, which follows a moving
 * value once it settles, and at the latest after a maximum latency. the
 * timer is simulated as the GUI's main loop runs it: whenever the clock
 * passes the policy's deadline before the next report
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include <cmocka.h>

#include "gui/presenter_target.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define MS 1000
#define QUIET (1000 * MS)
#define MAX_LATENCY (3000 * MS)

typedef struct _sim_t
{
  dt_presenter_target_t target;
  int64_t now;
  // target changes, when the last one happened, and the longest time
  // between a value's first report differing from the target and the
  // target changing
  int changes;
  int64_t changed_at;
} _sim_t;

static void _sim_init(_sim_t *s)
{
  dt_presenter_target_init(&s->target);
  s->now = 0;
  s->changes = 0;
  s->changed_at = -1;
}

// the main loop: fire the timer at every deadline up to t
static void _advance(_sim_t *s,
                     const int64_t t)
{
  for(;;)
  {
    const int64_t deadline = dt_presenter_target_deadline(&s->target);
    if(!deadline || deadline > t) break;
    assert_true(deadline >= s->now);
    s->now = deadline;
    if(dt_presenter_target_tick(&s->target, deadline))
    {
      s->changes++;
      s->changed_at = deadline;
    }
  }
  s->now = t;
}

static void _report(_sim_t *s,
                    const int64_t t,
                    const float value,
                    const float ceiling,
                    const uint32_t generation)
{
  _advance(s, t);
  const dt_presenter_headroom_t r = { .value = value, .ceiling = ceiling,
                                      .generation = generation };
  if(dt_presenter_target_report(&s->target, &r, t))
  {
    s->changes++;
    s->changed_at = t;
  }
}

static void _value(_sim_t *s,
                   const int64_t t,
                   const float value)
{
  _report(s, t, value, 0.0f, 0);
}

static float _headroom(const _sim_t *s)
{
  return dt_presenter_target_headroom(&s->target);
}

static float _ev(const float ev)
{
  return exp2f(ev);
}

/* quantization */

static void test_steps_exact(void **state)
{
  // exact 1/8 EV steps keep their step despite float error, both as
  // exp2f() gives them and as 203-nit multiples rounded to float
  for(int k = 0; k <= 48; k++)
  {
    const float v = exp2f(k / 8.0f);
    assert_int_equal(dt_presenter_target_steps(v), k);
    assert_int_equal(dt_presenter_target_steps((float)(203.0 * pow(2.0, k / 8.0)) / 203.0f), k);
    // and a hair below a step is the step below it
    if(k > 0) assert_int_equal(dt_presenter_target_steps(v * 0.999f), k - 1);
  }
  // the target given back for a step is that step again
  dt_presenter_target_t t;
  for(int k = 0; k <= 48; k++)
  {
    dt_presenter_target_init(&t);
    t.steps = k;
    assert_int_equal(dt_presenter_target_steps(dt_presenter_target_headroom(&t)), k);
  }
}

static void test_steps_round_down(void **state)
{
  // SDR and below, and nonsense, are 1.0
  assert_int_equal(dt_presenter_target_steps(1.0f), 0);
  assert_int_equal(dt_presenter_target_steps(0.5f), 0);
  assert_int_equal(dt_presenter_target_steps(0.0f), 0);
  assert_int_equal(dt_presenter_target_steps(-3.0f), 0);
  assert_int_equal(dt_presenter_target_steps(NAN), 0);
  assert_int_equal(dt_presenter_target_steps(INFINITY), 16 * 8);
  // 1000 nits over 203: log2 = 2.30, step 18 = 2.25 EV, 966 nits
  assert_int_equal(dt_presenter_target_steps(1000.0f / 203.0f), 18);
  // one nit above reference stays SDR; 221 nits is just below 2^(1/8)
  assert_int_equal(dt_presenter_target_steps(204.0f / 203.0f), 0);
  assert_int_equal(dt_presenter_target_steps(221.0f / 203.0f), 0);
  assert_int_equal(dt_presenter_target_steps(222.0f / 203.0f), 1);
  // mutter's PQ default, 10000 over 203
  assert_int_equal(dt_presenter_target_steps(10000.0f / 203.0f), 44);

  // a 1-nit sweep from 203 to 1600 nits gives 24 targets, as in review
  int distinct = 0, last = -1;
  for(int nits = 203; nits <= 1600; nits++)
  {
    const int s = dt_presenter_target_steps((float)nits / 203.0f);
    if(s != last) distinct++;
    last = s;
  }
  assert_int_equal(distinct, 24);
}

/* the first value, the ceiling and generations */

static void test_first_value_at_once(void **state)
{
  _sim_t s;
  _sim_init(&s);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  _value(&s, 0, 1000.0f / 203.0f);
  assert_int_equal(s.changes, 1);
  assert_int_equal(s.changed_at, 0);
  assert_float_equal(_headroom(&s), exp2f(18 / 8.0f), 1e-6f);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
}

static void test_ceiling_bounds(void **state)
{
  _sim_t s;
  _sim_init(&s);
  // macOS before EDR is engaged: no value, the potential as ceiling. the
  // ceiling is no target: 1.0 until a value comes
  _report(&s, 0, NAN, 16.0f, 1);
  assert_int_equal(s.changes, 0);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  // values above the ceiling are the ceiling
  _report(&s, 10 * MS, 20.0f, 16.0f, 1);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 16.0f, 1e-5f);
  _report(&s, 20 * MS, 20.0f, 16.0f, 1);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 1);

  // a Pro Display XDR at 500 nits of reference: 3.2 now of a potential
  // 400, the current value taken at once
  _sim_init(&s);
  _report(&s, 0, 3.2f, 400.0f, 1);
  assert_int_equal(s.changes, 1);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)), dt_presenter_target_steps(3.2f));
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 1);
}

static void test_generation_resets(void **state)
{
  _sim_t s;
  _sim_init(&s);
  _report(&s, 0, 4.0f, 0.0f, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);

  // within a generation a lower value settles first
  _report(&s, 1000 * MS, 2.0f, 0.0f, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);
  // another screen: taken at once, the level waiting to settle forgotten
  _report(&s, 1100 * MS, 3.0f, 0.0f, 2);
  assert_int_equal(s.changes, 2);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)), dt_presenter_target_steps(3.0f));
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 5000 * MS);
  assert_int_equal(s.changes, 2);

  // a value inside the band of the old target, which the hysteresis would
  // keep within a generation, is taken as it is in a new one
  _sim_init(&s);
  _report(&s, 0, 4.0f, 0.0f, 1);
  _report(&s, 100 * MS, 4.0f * _ev(0.17f), 0.0f, 1);
  _advance(&s, 5000 * MS);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);
  _report(&s, 5000 * MS, 4.0f * _ev(0.17f), 0.0f, 2);
  assert_float_equal(_headroom(&s), 4.0f * _ev(0.125f), 1e-5f);

  // a new generation that keeps the quantized target still drops the
  // level waiting to settle in the old one
  _sim_init(&s);
  _report(&s, 0, 4.0f, 0.0f, 1);
  _report(&s, 100 * MS, 2.0f, 0.0f, 1);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 1100 * MS);
  _report(&s, 200 * MS, 4.0f, 16.0f, 2);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 5000 * MS);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);

  // and an SDR target kept by a ceiling of 1
  _sim_init(&s);
  _report(&s, 0, 1.0f, 0.0f, 1);
  _report(&s, 100 * MS, 2.0f, 0.0f, 1);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 1100 * MS);
  _report(&s, 200 * MS, 4.0f, 1.0f, 2);
  assert_int_equal(s.changes, 0);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 5000 * MS);
  assert_int_equal(s.changes, 0);

  // a new generation without a value but with a ceiling: 1.0, the level
  // waiting to settle dropped
  _sim_init(&s);
  _report(&s, 0, 4.0f, 0.0f, 1);
  _report(&s, 100 * MS, 2.0f, 0.0f, 1);
  _report(&s, 200 * MS, NAN, 16.0f, 2);
  assert_int_equal(s.changes, 2);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 5000 * MS);
  assert_int_equal(s.changes, 2);

  // a window moved back to a screen whose EDR is still engaged: the
  // first value of the new generation is taken at once, not the potential
  _sim_init(&s);
  _report(&s, 0, 2.0f, 0.0f, 1);
  _report(&s, 1000 * MS, 3.998f, 16.0f, 2);
  assert_int_equal(s.changes, 2);
  assert_int_equal(s.target.steps, 15);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _report(&s, 1010 * MS, 3.998f, 16.0f, 2);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 2);

  // a new generation without a value: an HDR target does not follow the
  // window onto a screen that may not show it. 1.0 until the first value
  _sim_init(&s);
  _report(&s, 0, 16.0f, 16.0f, 1);
  assert_float_equal(_headroom(&s), 16.0f, 1e-5f);
  _report(&s, 1000 * MS, NAN, 0.0f, 2);
  assert_int_equal(s.changes, 2);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  _report(&s, 1100 * MS, NAN, 0.0f, 2);
  assert_int_equal(s.changes, 2);
  _report(&s, 1200 * MS, 2.0f, 0.0f, 2);
  assert_int_equal(s.changes, 3);
  assert_int_equal(s.changed_at, 1200 * MS);
  assert_float_equal(_headroom(&s), 2.0f, 1e-6f);
}

static void test_no_value_keeps(void **state)
{
  _sim_t s;
  _sim_init(&s);
  _value(&s, 0, 4.0f);
  // the display sleeps: nothing changes, however long
  for(int k = 1; k <= 100; k++)
    _report(&s, k * 100 * MS, NAN, 0.0f, 0);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);

  // a level waiting to settle is dropped by a report without a value
  _value(&s, 20000 * MS, 2.0f);
  assert_true(dt_presenter_target_deadline(&s.target) > 0);
  _report(&s, 20100 * MS, NAN, 0.0f, 0);
  assert_int_equal(dt_presenter_target_deadline(&s.target), 0);
  _advance(&s, 30000 * MS);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);

  // an infinite value is no value either
  _value(&s, 31100 * MS, INFINITY);
  _advance(&s, 40000 * MS);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);

  // no value at first: 1.0 until one comes, which is taken at once
  _sim_init(&s);
  _report(&s, 0, NAN, 0.0f, 0);
  assert_float_equal(_headroom(&s), 1.0f, 0.0f);
  _value(&s, 500 * MS, 2.0f);
  assert_float_equal(_headroom(&s), 2.0f, 1e-6f);
  assert_int_equal(s.changed_at, 500 * MS);
}

/* settling */

static void test_jump(void **state)
{
  _sim_t s;
  _sim_init(&s);
  _value(&s, 0, 2.0f);
  _value(&s, 1000 * MS, 8.0f);
  _advance(&s, 1000 * MS + QUIET - 1);
  assert_float_equal(_headroom(&s), 2.0f, 1e-6f);
  _advance(&s, 1000 * MS + QUIET);
  assert_float_equal(_headroom(&s), 8.0f, 1e-5f);
  assert_int_equal(s.changes, 2);

  // the same value reported again does not postpone it
  _sim_init(&s);
  _value(&s, 0, 2.0f);
  for(int k = 0; k < 10; k++) _value(&s, 1000 * MS + k * 16 * MS, 8.0f);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changed_at, 1000 * MS + QUIET);

  // a jump back before it settled costs nothing
  _sim_init(&s);
  _value(&s, 0, 2.0f);
  _value(&s, 1000 * MS, 8.0f);
  _value(&s, 1200 * MS, 2.0f);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 1);
}

// macOS EDR as probed in review of #22953: 1.0 until EDR content is shown
// (1.2 on a Pro Display XDR), then 1.27, 2.45, 4.89, 9.78, 16.0 over about
// 2 s, one notification every 8 ms, about 230 of them
static int _edr_ramp(_sim_t *s,
                     const int64_t start,
                     const float from,
                     const float ceiling)
{
  const float points[] = { from, 1.27f, 2.45f, 4.89f, 9.78f, 16.0f };
  const int n = 230;
  for(int k = 0; k <= n; k++)
  {
    // log-linear between the probed values, which come 0.4 s apart
    const float x = 5.0f * k / n;
    const int i = MIN((int)x, 4);
    const float f = x - i;
    const float v = exp2f(log2f(points[i]) * (1.0f - f) + log2f(points[i + 1]) * f);
    _report(s, start + (int64_t)k * 2000 * MS / n, v, ceiling, 1);
  }
  return n;
}

static void test_edr_ramp(void **state)
{
  // cold start: no value before EDR is engaged, then the idle level once
  // an image is shown, taken at once, and the ramp from there costs one
  // change, when it has ended
  _sim_t s;
  _sim_init(&s);
  _report(&s, 0, NAN, 16.0f, 1);
  assert_int_equal(s.changes, 0);
  _edr_ramp(&s, 100 * MS, 1.2f, 16.0f);
  assert_int_equal(s.changes, 1);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 2);
  assert_float_equal(_headroom(&s), 16.0f, 1e-5f);
  assert_int_equal(s.changed_at, 2100 * MS + QUIET);

  // without a ceiling, from 1.0: the same
  _sim_init(&s);
  _report(&s, 0, 1.0f, 0.0f, 1);
  assert_int_equal(s.changes, 0);
  _edr_ramp(&s, 100 * MS, 1.0f, 0.0f);
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 1);
  assert_float_equal(_headroom(&s), 16.0f, 1e-5f);
  assert_int_equal(s.changed_at, 2100 * MS + QUIET);
}

// KWin's HDR brightness: the reference luminance follows an animation, a
// report per frame, so the headroom (peak over reference) moves in small
// steps. a drag from 4x to 2.5x over 0.33 s at 60 Hz, then kscreen-doctor
// style steps of 10 nits of reference a second apart
static void test_kwin_brightness(void **state)
{
  _sim_t s;
  _sim_init(&s);
  const float peak = 1000.0f;
  _value(&s, 0, peak / 250.0f);
  const int frames = 20;
  for(int k = 1; k <= frames; k++)
  {
    const float reference = 250.0f + (400.0f - 250.0f) * k / frames;
    // whole nits, as the protocol sends them
    _value(&s, 1000 * MS + k * 16667, peak / roundf(reference));
  }
  const int64_t last = 1000 * MS + frames * 16667;
  _advance(&s, 10000 * MS);
  assert_int_equal(s.changes, 2);
  assert_int_equal(s.changed_at, last + QUIET);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)),
                   dt_presenter_target_steps(peak / 400.0f));

  // steps of 10 nits: 0.04 to 0.06 EV each, below a step. each one
  // animates over a few frames
  _sim_init(&s);
  _value(&s, 0, peak / 200.0f);
  int64_t t = 1000 * MS;
  for(int reference = 200; reference < 300; reference += 10)
  {
    for(int f = 1; f <= 5; f++)
      _value(&s, t + f * 16667, peak / (reference + 2.0f * f));
    t += 1000 * MS;
  }
  _advance(&s, t + 10000 * MS);
  // 200 -> 300 nits is 0.58 EV: at most one change per 1/8 EV crossed,
  // far fewer than the 50 reports
  assert_true(s.changes <= 1 + 5);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)),
                   dt_presenter_target_steps(peak / 300.0f));
}

// kscreen-doctor's sdr-brightness on KWin 6.8, as measured: 10 steps a
// person makes 0.35 s apart, each one level down. KWin's animation of a
// step ends well inside the gap, so a quiet time shorter than the gap
// takes every step as settled
static void test_kwin_steps(void **state)
{
  _sim_t s;
  _sim_init(&s);
  _value(&s, 0, _ev(3.0f));
  const int n = 10;
  const int64_t start = 1000 * MS, gap = 350 * MS;
  for(int k = 0; k < n - 1; k++)
    _value(&s, start + k * gap, _ev(3.0f - (k + 1) / 8.0f));
  // the steps span 3.15 s, more than the maximum latency, which takes the
  // level reached by then
  _advance(&s, start + MAX_LATENCY);
  assert_int_equal(s.changes, 1 + 1);
  assert_int_equal(s.changed_at, start + MAX_LATENCY);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)), 24 - (n - 1));
  // and the last step a quiet time after it
  const int64_t last = start + (n - 1) * gap;
  _value(&s, last, _ev(3.0f - n / 8.0f));
  _advance(&s, last + 10000 * MS);
  assert_int_equal(s.changes, 1 + 2);
  assert_int_equal(s.changed_at, last + QUIET);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)), 24 - n);
}

static void test_max_latency(void **state)
{
  // a value that never stops moving (automatic brightness) still reaches
  // the target, at the latest every maximum latency
  _sim_t s;
  _sim_init(&s);
  _value(&s, 0, 8.0f);
  int64_t first_differing = -1;
  int64_t worst = 0;
  int changes = s.changes;
  for(int k = 1; k <= 1000; k++)
  {
    const int64_t t = (int64_t)k * 10 * MS;
    // down 0.3 EV per second
    const float v = 8.0f * exp2f(-0.3f * t / 1e6f);
    const int before = s.target.steps;
    _value(&s, t, v);
    if(first_differing < 0 && dt_presenter_target_deadline(&s.target)) first_differing = t;
    if(s.changes != changes)
    {
      assert_true(s.target.steps < before || s.changed_at < t);
      worst = MAX(worst, s.changed_at - first_differing);
      first_differing = dt_presenter_target_deadline(&s.target) ? t : -1;
      changes = s.changes;
    }
  }
  assert_true(worst <= MAX_LATENCY);
  // 10 s of drift: a change every 3 s, not one per 1/8 EV (24 of them)
  assert_int_equal(s.changes, 1 + 3);
  // and the last value once it stops
  _advance(&s, 20000 * MS);
  assert_int_equal(dt_presenter_target_steps(_headroom(&s)),
                   dt_presenter_target_steps(8.0f * exp2f(-3.0f)));
}

static void test_hysteresis(void **state)
{
  // a value sitting on a step boundary with noise flips once, down. the
  // noise comes slower than the quiet time, or the level below would
  // never settle
  _sim_t s;
  _sim_init(&s);
  _value(&s, 0, 4.0f);
  for(int k = 1; k <= 200; k++)
  {
    const float noise = (k % 2 ? 0.3f : -0.3f) / 8.0f;
    _value(&s, k * 2 * QUIET, 4.0f * _ev(noise));
  }
  _advance(&s, 202 * 2 * QUIET);
  assert_int_equal(s.changes, 2);
  assert_float_equal(_headroom(&s), 4.0f * _ev(-0.125f), 1e-5f);

  // going up needs half a step over the next one
  _sim_init(&s);
  _value(&s, 0, 4.0f);
  _value(&s, 1000 * MS, 4.0f * _ev(1.4f / 8.0f));
  _advance(&s, 10000 * MS);
  assert_float_equal(_headroom(&s), 4.0f, 1e-6f);
  _value(&s, 10000 * MS, 4.0f * _ev(1.5f / 8.0f));
  _advance(&s, 20000 * MS);
  assert_float_equal(_headroom(&s), 4.0f * _ev(0.125f), 1e-5f);

  // and going down none: below the target the screen shows less than the
  // pipes render for
  _sim_init(&s);
  _value(&s, 0, 4.0f);
  _value(&s, 1000 * MS, 4.0f * 0.999f);
  _advance(&s, 10000 * MS);
  assert_float_equal(_headroom(&s), 4.0f * _ev(-0.125f), 1e-5f);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_steps_exact),
    cmocka_unit_test(test_steps_round_down),
    cmocka_unit_test(test_first_value_at_once),
    cmocka_unit_test(test_ceiling_bounds),
    cmocka_unit_test(test_generation_resets),
    cmocka_unit_test(test_no_value_keeps),
    cmocka_unit_test(test_jump),
    cmocka_unit_test(test_edr_ramp),
    cmocka_unit_test(test_kwin_brightness),
    cmocka_unit_test(test_kwin_steps),
    cmocka_unit_test(test_max_latency),
    cmocka_unit_test(test_hysteresis),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
