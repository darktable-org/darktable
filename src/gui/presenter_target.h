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

#pragma once

/*
 * internal to the presenter: the HDR headroom a window's pipes render for.
 * a backend reports what the platform says the screen can show; the
 * presenter core turns those reports into a target that changes rarely,
 * since every change reruns the window's pipes from the start. the policy
 * is pure, with the clock passed in, so that it can be tested. see
 * dev-doc/display_presentation.md, "Headroom target".
 */

#include <glib.h>
#include <stdint.h>

// what a backend reports: never given to pipes as it is
typedef struct dt_presenter_headroom_t
{
  // the screen's peak over reference white now, NAN when the platform
  // has no value: the display sleeps, or what it gives says nothing yet
  // (macOS reads 1.0 before EDR is engaged)
  float value;
  // the most the screen can show in this window, 0 if the platform does
  // not say (macOS's potential EDR headroom)
  float ceiling;
  // a new generation says that earlier reports tell nothing about this
  // one: another screen or mode, a display woken up
  uint32_t generation;
} dt_presenter_headroom_t;

typedef struct dt_presenter_target_t
{
  // the target, in steps of 1/DT_PRESENTER_TARGET_STEPS EV above 1.0
  int steps;
  // a report arrived, of this generation
  gboolean started;
  uint32_t generation;
  // the next known value is taken at once: a generation began without a
  // ceiling to stand in for it
  gboolean fresh;
  // a level differing from the target, waiting to settle: the level, the
  // last value reported, when the first report of it and when the last
  // change of the value arrived (microseconds)
  gboolean candidate;
  int candidate_steps;
  float last_value;
  int64_t first, changed;
} dt_presenter_target_t;

#define DT_PRESENTER_TARGET_STEPS 8

/** a target of 1.0, no report yet */
void dt_presenter_target_init(dt_presenter_target_t *target);
/** take a report arriving at now (microseconds, monotonic); TRUE if the
 * target changed */
gboolean dt_presenter_target_report(dt_presenter_target_t *target,
                                    const dt_presenter_headroom_t *report,
                                    const int64_t now);
/** the time passed by: TRUE if the target changed */
gboolean dt_presenter_target_tick(dt_presenter_target_t *target,
                                  const int64_t now);
/** when dt_presenter_target_tick() has something to do, or 0 */
int64_t dt_presenter_target_deadline(const dt_presenter_target_t *target);
/** the headroom pipes render for, 1.0 at reference white */
float dt_presenter_target_headroom(const dt_presenter_target_t *target);
/** the steps of headroom: log2 rounded down to 1/8 EV, at least 0 */
int dt_presenter_target_steps(const float headroom);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
