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
  // FIXME: rejigger so that NONE is first, and test for NONE case throughout
  DT_LIB_COLORPICKER_SIZE_POINT = 0,
  DT_LIB_COLORPICKER_SIZE_BOX,
  // FIXME: instead just set picker to NULL for activate IOP?
  DT_LIB_COLORPICKER_SIZE_NONE
} dt_lib_colorpicker_size_t;

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
  // NOTE: locked only applies to live samples
  gboolean locked;

  /** The actual picked colors */
  // in display profile, as picked from preview pixelpipe
  dt_aligned_pixel_t picked_color_display_rgb_mean;
  dt_aligned_pixel_t picked_color_display_rgb_min;
  dt_aligned_pixel_t picked_color_display_rgb_max;

  // converted display profile -> histogram profile
  dt_aligned_pixel_t picked_color_rgb_mean;
  dt_aligned_pixel_t picked_color_rgb_min;
  dt_aligned_pixel_t picked_color_rgb_max;

  // converted display profile -> Lab
  dt_aligned_pixel_t picked_color_lab_mean;
  dt_aligned_pixel_t picked_color_lab_min;
  dt_aligned_pixel_t picked_color_lab_max;

  // mean, min, or max color for tooltip in histogram profile
  int rgb_vals[4];

  /** The GUI elements */
  GtkWidget *container;
  GtkWidget *color_patch;
  GtkWidget *output_label;

  // sample in current mode (mean/min/max) in display and histogram colorspace
  GdkRGBA rgb_display;
} dt_colorpicker_sample_t;

typedef enum dt_lib_colorpicker_statistic_t
{
  DT_LIB_COLORPICKER_STATISTIC_MEAN = 0,
  DT_LIB_COLORPICKER_STATISTIC_MIN,
  DT_LIB_COLORPICKER_STATISTIC_MAX,
  DT_LIB_COLORPICKER_STATISTIC_N // needs to be the lsat one
} dt_lib_colorpicker_statistic_t;


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
