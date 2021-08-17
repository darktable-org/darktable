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
  DT_LIB_COLORPICKER_SIZE_BOX
} dt_lib_colorpicker_size_t;

/** The struct for primary and live color picker samples */
// FIXME: for primary and live sample we need all this data -- for per-module picker we need thje picked coor data for that point in the pixelpipe, but could lose point/box/size/locked 
typedef struct dt_colorpicker_sample_t
{
  /** The sample area or point */
  float point[2];
  dt_boundingbox_t box;
  dt_lib_colorpicker_size_t size;
  // FIXME: change this to "active" -- for primary picker it's true once the picker has been turned on, until the picker is reset -- for live pickers it corresponds to "locked" setting
  // FIXME: when user clicks off the colorpicker button in the primary picker, this should turn off -- if a module colorpicker is selected, there should still be a transparent way to click this off (clicking off the per-module picker, or perhaps the primary picker remains selected, and clicking it turns off per-module picker as well
  gboolean locked;
  // FIXME: instead of setting/testing this, check "dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF"
  gboolean active;

  /** The actual picked colors */
  dt_aligned_pixel_t picked_color_rgb_mean;
  dt_aligned_pixel_t picked_color_rgb_min;
  dt_aligned_pixel_t picked_color_rgb_max;

  dt_aligned_pixel_t picked_color_lab_mean;
  dt_aligned_pixel_t picked_color_lab_min;
  dt_aligned_pixel_t picked_color_lab_max;

  /** The GUI elements */
  // FIXME: these may be only for primary and live pickers -- keep local to colorpicker.c if so?
  GtkWidget *container;
  GtkWidget *color_patch;
  GtkWidget *output_label;
  GdkRGBA rgb;
} dt_colorpicker_sample_t;

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
