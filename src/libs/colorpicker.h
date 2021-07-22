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

#define DT_COLORPICKER_SIZE_POINT 0
#define DT_COLORPICKER_SIZE_BOX 1

/** The struct for live color picker samples */
typedef struct dt_colorpicker_sample_t
{
  /** The sample area or point */
  float point[2];
  dt_boundingbox_t box;
  int size;
  int locked;

  /** The actual picked colors */
  dt_aligned_pixel_t picked_color_rgb_mean;
  dt_aligned_pixel_t picked_color_rgb_min;
  dt_aligned_pixel_t picked_color_rgb_max;

  dt_aligned_pixel_t picked_color_lab_mean;
  dt_aligned_pixel_t picked_color_lab_min;
  dt_aligned_pixel_t picked_color_lab_max;

  /** The GUI elements */
  GtkWidget *container;
  GtkWidget *color_patch;
  GtkWidget *output_label;
  GdkRGBA rgb;
} dt_colorpicker_sample_t;

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
