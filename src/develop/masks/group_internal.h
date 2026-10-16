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

// the mask operators of the group compositor (masks/group.c), the arithmetic
// behind every operator the panel offers. Declared here so that the compositor
// tests can check them on small buffers with known values: the design relies
// on what they guarantee, such as which operators are order-independent, that
// an empty group is the identity of its operator (so that an empty intersect
// group does not blank the mask), and that opacity and inversion compose the
// same way for every operator.
//
// Not public API: everything outside group.c composites through
// dt_masks_group_render_roi()

#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

/* Composite `newmask` into the `dest` accumulator, in place, over `npixels`.
   `opacity` scales the incoming mask; `inverted` complements it first (i.e.
   uses 1 - newmask). dest is both input and output. */

void dt_masks_combine_maximum(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted);
void dt_masks_combine_minimum(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted);
void dt_masks_combine_difference(float *const restrict dest,
                                 float *const restrict newmask,
                                 const size_t npixels,
                                 const float opacity,
                                 const int inverted);
void dt_masks_combine_sum(float *const restrict dest,
                          float *const restrict newmask,
                          const size_t npixels,
                          const float opacity,
                          const int inverted);
void dt_masks_combine_exclusion(float *const restrict dest,
                                float *const restrict newmask,
                                const size_t npixels,
                                const float opacity,
                                const int inverted);
void dt_masks_combine_product(float *const restrict dest,
                              float *const restrict newmask,
                              const size_t npixels,
                              const float opacity,
                              const int inverted);
void dt_masks_combine_screen(float *const restrict dest,
                             float *const restrict newmask,
                             const size_t npixels,
                             const float opacity,
                             const int inverted);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
