/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#ifndef DARKTABLE_IOP_COLOR_H
#define DARKTABLE_IOP_COLOR_H

#include <lcms2.h>

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 100

#define LUT_SAMPLES 0x10000

// constants fit to the ones from lcms.h:
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL = INTENT_PERCEPTUAL,                       // 0
  DT_INTENT_RELATIVE_COLORIMETRIC = INTENT_RELATIVE_COLORIMETRIC, // 1
  DT_INTENT_SATURATION = INTENT_SATURATION,                       // 2
  DT_INTENT_ABSOLUTE_COLORIMETRIC = INTENT_ABSOLUTE_COLORIMETRIC  // 3
} dt_iop_color_intent_t;

typedef struct dt_iop_color_profile_t
{
  char filename[512]; // icc file name
  char name[512];     // product name
  int pos;            // position in combo box
  int display_pos;    // position in display combo box
} dt_iop_color_profile_t;

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
