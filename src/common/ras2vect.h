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

/* Returns a list of path forms after having vectorized the raster mask.
   The coordinates are either on mask space: (0 x 0) -> (width x height)
   or if image is set (not NULL) on image spaces making the masks directly
   usable on the corresponding image.

   cleanup   – potrace turdsize: area of largest speckle to suppress (default 2).
   smoothing – potrace alphamax: corner threshold (0 = all sharp, 1.0 = balanced,
               1.3 = maximum smoothing). Higher = fewer control points.

   If out_signs is not NULL, a parallel GList of GINT_TO_POINTER is
   returned: '+' for outer boundaries, '-' for holes.
   The caller must free this list with g_list_free().
*/
GList *ras2forms(const float *mask,
                 const int width,
                 const int height,
                 const dt_image_t *const image,
                 const int cleanup,
                 const double smoothing,
                 GList **out_signs);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
