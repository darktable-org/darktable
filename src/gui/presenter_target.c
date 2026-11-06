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

#include "gui/presenter_target.h"

#include <math.h>

/*
 * no surveyed editor follows the screen's headroom: Lightroom, Resolve and
 * Krita fix their target. viewers re-fit every frame on the GPU, and
 * Chromium still throttles headroom-only changes on macOS to 12 Hz. a
 * darktable target change reruns a whole edit, so it settles. the
 * constants are hypotheses; dev-doc/display_presentation.md says how to
 * measure them.
 */

// a level is floor(log2(headroom) * 8) / 8. 1/8 EV moves the tone
// mapper's white by 9% at most, and the platform clips or fits that much
// until the target follows. the epsilon keeps a value at an exact step
// from losing it to float error (as proposed in review of #22953)
#define STEP_EPSILON 1e-4f

// a level above the target counts only once the value clears it by half
// a step. rounding down needs no margin below: under the target's level
// the screen shows less than the pipes render for, so that level is left
// as soon as it settles. a value sitting on a step boundary then flips at
// most once
#define RISE_MARGIN 0.5f

// a level is taken once the reported value stopped changing for this
// long: longer than the gaps between brightness steps a person makes
// (0.35 s on KWin 6.8, whose animation of a step ends well inside one)
// and the pauses of a slider drag. a single change lands about 1 s late,
// mostly hidden by KWin's own dimming
#define QUIET_US (1000 * 1000)

// and at the latest this long after the first report of a new level, so
// that a value that keeps moving (KWin's automatic brightness drifts for
// seconds) still reaches the image. longer than macOS's EDR ramp from 1.0
// to its potential, about 2 s, so that a ramp toward a target taken from
// the ceiling costs no intermediate one
#define MAX_LATENCY_US (3 * 1000 * 1000)

// 2^16: no screen comes near, and a bogus value stays in int range
#define MAX_STEPS (16 * DT_PRESENTER_TARGET_STEPS)

static float _log_steps(const float headroom)
{
  return DT_PRESENTER_TARGET_STEPS * log2f(headroom);
}

int dt_presenter_target_steps(const float headroom)
{
  if(!(headroom > 1.0f)) return 0;
  const float steps = floorf(_log_steps(headroom) + STEP_EPSILON);
  return steps < MAX_STEPS ? (int)steps : MAX_STEPS;
}

void dt_presenter_target_init(dt_presenter_target_t *target)
{
  *target = (dt_presenter_target_t){ .steps = 0 };
}

static gboolean _set(dt_presenter_target_t *target,
                     const int steps)
{
  target->candidate = FALSE;
  if(steps == target->steps) return FALSE;
  target->steps = steps;
  return TRUE;
}

int64_t dt_presenter_target_deadline(const dt_presenter_target_t *target)
{
  if(!target->candidate) return 0;
  return MIN(target->changed + QUIET_US, target->first + MAX_LATENCY_US);
}

gboolean dt_presenter_target_tick(dt_presenter_target_t *target,
                                  const int64_t now)
{
  if(!target->candidate || now < dt_presenter_target_deadline(target)) return FALSE;
  return _set(target, target->candidate_steps);
}

gboolean dt_presenter_target_report(dt_presenter_target_t *target,
                                    const dt_presenter_headroom_t *report,
                                    const int64_t now)
{
  gboolean changed = FALSE;
  const float ceiling = isfinite(report->ceiling) && report->ceiling > 0.0f
                        ? report->ceiling : 0.0f;

  // what an earlier generation settled on says nothing about this one,
  // the hysteresis around it included. a ceiling is the provisional
  // target, what a platform that ramps up (macOS EDR) ends at or below;
  // without one the first value is taken as it comes
  if(!target->started || report->generation != target->generation)
  {
    target->started = TRUE;
    target->generation = report->generation;
    target->candidate = FALSE;
    target->fresh = ceiling == 0.0f;
    if(ceiling > 0.0f) changed = _set(target, dt_presenter_target_steps(ceiling));
  }

  // no value now: what was settled stays, and a level waiting to settle
  // is no longer what the platform says
  if(!isfinite(report->value))
  {
    target->candidate = FALSE;
    return changed;
  }

  const float value = ceiling > 0.0f ? MIN(report->value, ceiling) : report->value;
  const int steps = dt_presenter_target_steps(value);

  if(target->fresh)
  {
    target->fresh = FALSE;
    return _set(target, steps) || changed;
  }

  const gboolean rises = value > 1.0f
    && _log_steps(value) + STEP_EPSILON >= target->steps + 1 + RISE_MARGIN;
  if(steps >= target->steps && !rises)
  {
    // back within the target's band
    target->candidate = FALSE;
    return changed;
  }

  if(!target->candidate)
  {
    target->candidate = TRUE;
    target->first = target->changed = now;
  }
  else if(value != target->last_value)
    target->changed = now;
  target->candidate_steps = steps;
  target->last_value = value;

  return dt_presenter_target_tick(target, now) || changed;
}

float dt_presenter_target_headroom(const dt_presenter_target_t *target)
{
  return exp2f((float)target->steps / DT_PRESENTER_TARGET_STEPS);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
