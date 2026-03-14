/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

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

#include "common/darktable.h"

G_BEGIN_DECLS

typedef enum _color_harmony_type_t
{
  DT_COLOR_HARMONY_NONE = 0,
  DT_COLOR_HARMONY_MONOCHROMATIC,
  DT_COLOR_HARMONY_ANALOGOUS,
  DT_COLOR_HARMONY_ANALOGOUS_COMPLEMENTARY,
  DT_COLOR_HARMONY_COMPLEMENTARY,
  DT_COLOR_HARMONY_SPLIT_COMPLEMENTARY,
  DT_COLOR_HARMONY_DYAD,
  DT_COLOR_HARMONY_TRIAD,
  DT_COLOR_HARMONY_TETRAD,
  DT_COLOR_HARMONY_SQUARE,
  DT_COLOR_HARMONY_N // needs to be the last one
} dt_color_harmony_type_t;

typedef enum dt_color_harmony_width_t
{
  DT_COLOR_HARMONY_WIDTH_NORMAL = 0,
  DT_COLOR_HARMONY_WIDTH_LARGE,
  DT_COLOR_HARMONY_WIDTH_NARROW,
  DT_COLOR_HARMONY_WIDTH_LINE,
  DT_COLOR_HARMONY_WIDTH_N // needs to be the last one
} dt_color_harmony_width_t;

typedef struct _color_harmony_t
{
  dt_color_harmony_type_t type;
  int rotation;
  dt_color_harmony_width_t width;
  // When custom_n > 0, draw these absolute-angle nodes instead of the type+rotation table.
  // Angles are normalized turns [0, 1), matching normalized UCS hue.
  int   custom_n;
  float custom_angles[4];
} dt_color_harmony_guide_t;

typedef int32_t dt_harmony_guide_id_t;

void dt_color_harmony_init(dt_color_harmony_guide_t *layout);

void dt_color_harmony_set(const dt_imgid_t imgid,
                          const dt_color_harmony_guide_t layout);

// Returns FALSE if no harmony is recorded for imgid.
gboolean dt_color_harmony_get(const dt_imgid_t imgid,
                              dt_color_harmony_guide_t *layout);

G_END_DECLS

// Returns the absolute RYB node positions (normalized turns [0,1)) for a predefined
// harmony type and anchor rotation (integer degrees). n is set to the node count.
// This is the single authoritative geometry table used by both the vectorscope overlay
// and the processing pipeline; keeping it here avoids duplicating angle definitions.
static inline void dt_color_harmony_get_sector_angles(dt_color_harmony_type_t type,
                                                       int rotation,
                                                       float *angles, int *n)
{
  static const struct { int n; float offsets[4]; } table[DT_COLOR_HARMONY_N] = {
    { 0, {  0.f                                          } }, // NONE
    { 1, {  0.f/12.f                                     } }, // MONOCHROMATIC
    { 3, { -1.f/12.f,  0.f/12.f,  1.f/12.f              } }, // ANALOGOUS
    { 4, { -1.f/12.f,  0.f/12.f,  1.f/12.f,  6.f/12.f  } }, // ANALOGOUS_COMPLEMENTARY
    { 2, {  0.f/12.f,  6.f/12.f                         } }, // COMPLEMENTARY
    { 3, {  0.f/12.f,  5.f/12.f,  7.f/12.f              } }, // SPLIT_COMPLEMENTARY
    { 2, { -1.f/12.f,  1.f/12.f                         } }, // DYAD
    { 3, {  0.f/12.f,  4.f/12.f,  8.f/12.f              } }, // TRIAD
    { 4, { -1.f/12.f,  1.f/12.f,  5.f/12.f,  7.f/12.f  } }, // TETRAD
    { 4, {  0.f/12.f,  3.f/12.f,  6.f/12.f,  9.f/12.f  } }, // SQUARE
  };

  if(type <= DT_COLOR_HARMONY_NONE || type >= DT_COLOR_HARMONY_N) { *n = 0; return; }
  *n = table[type].n;
  const float anchor = rotation / 360.0f;
  for(int i = 0; i < *n; i++)
  {
    float a = table[type].offsets[i] + anchor;
    a -= floorf(a); // wrap to [0, 1), handles negative offsets
    angles[i] = a;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
