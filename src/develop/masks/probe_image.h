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

// a generated "probe" image to replay harvested masks on (see verify.h). The
// user's photo is never collected, and an image that leaves a mask empty would
// make the check vacuous: a parametric mask selecting the brightest tenth is
// empty on a dark picture, and equal to any other empty mask.
//
// Its coverage is set from the code and the color spaces, not from real edits,
// which would tune it to someone's habits and leave other users' ranges
// unverified: every channel blendif offers, over every value it can take.
// Covering the linear RGB cube densely covers every derived channel at once,
// through the pipeline's own color math. test_probe_image.c sweeps the cube to
// learn each channel's range, and holds the probe to it.
//
// Beyond color it needs:
//
//   - hard edges at several scales and texture at every wavelet octave:
//     feathering and detail masks read the image's structure, and are no-ops
//     on a smooth one
//   - even coverage everywhere, as a shape can sit anywhere: each tile sweeps
//     a full 2D slice of the cube, and neighboring tiles walk the other axes
//     on low-discrepancy sequences
//
// It is scene-referred linear RGB with values above 1 (a per-tile exposure
// ladder), as blendif boost factors reach there. It is deterministic (integer
// hash noise), so a harvest replays the same on every machine.

#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

/** allocate and fill the probe image: `width` * `height` pixels of 4 floats
    (linear scene-referred RGB, the 4th 0), the same for a given size. Free it
    with dt_free_align(). NULL if allocation fails or a size is not positive */
float *dt_masks_probe_new(const int width, const int height);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
