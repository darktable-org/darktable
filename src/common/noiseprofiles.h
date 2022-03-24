/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#include "common/image.h"
#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct dt_noiseprofile_t
{
  char *name;
  char *maker;
  char *model;
  int iso;
  dt_aligned_pixel_t a; // poissonian part; use 4 aligned instead of 3 elements to aid vectorization
  dt_aligned_pixel_t b; // gaussian part
}
dt_noiseprofile_t;

extern const dt_noiseprofile_t dt_noiseprofile_generic;

/** read the noiseprofile file once on startup (kind of)*/
JsonParser *dt_noiseprofile_init(const char *alternative);

/*
 * returns the noiseprofiles matching the image's exif data.
 * free with g_list_free_full(..., dt_noiseprofile_free);
 */
GList *dt_noiseprofile_get_matching(const dt_image_t *cimg);

/** convenience function to free a list of noiseprofiles */
void dt_noiseprofile_free(gpointer data);

/*
 * interpolate values from p1 and p2 into out.
 */
void dt_noiseprofile_interpolate(
  const dt_noiseprofile_t *const p1,  // the smaller iso
  const dt_noiseprofile_t *const p2,  // the larger iso (can't be == iso1)
  dt_noiseprofile_t *out);            // has iso initialized

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

