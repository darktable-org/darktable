/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include <gtk/gtk.h>
#include <inttypes.h>

typedef enum dt_lib_colorpicker_size_t
{
  DT_LIB_COLORPICKER_SIZE_POINT = 0,
  DT_LIB_COLORPICKER_SIZE_BOX,
} dt_lib_colorpicker_size_t;

typedef enum dt_lib_colorpicker_statistic_t
{
  DT_PICK_MEAN = 0,
  DT_PICK_MIN,
  DT_PICK_MAX,
  DT_PICK_N // needs to be the last one
} dt_lib_colorpicker_statistic_t;

typedef dt_aligned_pixel_t lib_colorpicker_stats[DT_PICK_N];

/** The struct for primary and live color picker samples */
typedef struct dt_colorpicker_sample_t
{
  /** The sample area or point */
  // For the primary sample, these are the current sample area,
  // whether from colorpicker lib or an iop. They are used for showing
  // the sample in the center view, and sampling in the pixelpipe.
  float point[2];
  dt_boundingbox_t box;
  dt_lib_colorpicker_size_t size;
  gboolean denoise;
  // NOTE: only applies to live samples
  gboolean locked;

  /** The actual picked colors */
  // picked color in display profile, as picked from preview pixelpipe
  lib_colorpicker_stats display;
  // picked color converted display profile -> histogram profile
  lib_colorpicker_stats scope;
  // picked color converted display profile -> Lab
  lib_colorpicker_stats lab;
  // in scope profile with current statistic
  int label_rgb[4];
  // in display profile with current statistic
  GdkRGBA swatch;

  /** The GUI elements */
  GtkWidget *container;
  GtkWidget *color_patch;
  GtkWidget *output_label;
} dt_colorpicker_sample_t;


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

