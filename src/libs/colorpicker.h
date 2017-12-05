/*
    This file is part of darktable,
    copyright (c) 2011 Robert Bieber.

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
  float box[4];
  int size;
  int locked;

  /** The actual picked colors */
  uint8_t picked_color_rgb_mean[3];
  uint8_t picked_color_rgb_min[3];
  uint8_t picked_color_rgb_max[3];

  float picked_color_lab_mean[3];
  float picked_color_lab_min[3];
  float picked_color_lab_max[3];

  /** The GUI elements */
  GtkWidget *container;
  GtkWidget *color_patch;
  GtkWidget *output_label;
  GtkWidget *delete_button;
  GdkRGBA rgb;
} dt_colorpicker_sample_t;

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
