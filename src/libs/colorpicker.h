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

// FIXME: if make a live sample type declared as "struct dt_lib_colorpicker_live_sample_t" can declare it in colorpicker.c

/** The struct for primary and live color picker samples */
// FIXME: for primary and live sample we need most of this data -- for per-module picker we need the picked coor data for that point in the pixelpipe, but could lose point/box/size/locked -- and some of this data is "private" to colorpicker.c and could move there
typedef struct dt_colorpicker_sample_t
{
  /** The sample area or point */
  // For the primary sample, these are the current sample area,
  // whether from colorpicker lib or an iop. They are used for showing
  // the sample in the center view, and sampling in the pixelpipe.
  float point[2];
  dt_boundingbox_t box;
  dt_lib_colorpicker_size_t size;
  // FIXME: this only applies to live samples
  gboolean locked;

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

// FIXME: should this be in a colorspace-specific utility header, e.g. iop_profile or colorspaces or colorspaces_inline_conversions
static inline gboolean dt_lib_colorpicker_convert_color_space(const GdkRGBA *restrict sample, GdkRGBA *restrict color)
{
  // RGB values are relative to the histogram color profile
  // we need to adapt them to display profile so color look right
  // Note : dt_ioppr_set_pipe_output_profile_info sets a non-handled output profile to sRGB by default
  // meaning that this conversion is wrong for fancy-pants LUT-based display profiles.

  dt_iop_order_iccprofile_info_t *histogram_profile = dt_ioppr_get_histogram_profile_info(darktable.develop);
  dt_iop_order_iccprofile_info_t *display_profile = dt_ioppr_get_pipe_output_profile_info(darktable.develop->pipe);

  dt_aligned_pixel_t RGB = { sample->red, sample->green, sample->blue };
  dt_aligned_pixel_t XYZ;

  if(!(histogram_profile && display_profile)) return TRUE; // no need to paint, color will be wrong

  // convert from histogram RGB to XYZ
  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ, histogram_profile->matrix_in_transposed, histogram_profile->lut_in,
                             histogram_profile->unbounded_coeffs_in, histogram_profile->lutsize,
                             histogram_profile->nonlinearlut);

  // convert from XYZ to display RGB
  dt_ioppr_xyz_to_rgb_matrix(XYZ, RGB, display_profile->matrix_out_transposed, display_profile->lut_out,
                             display_profile->unbounded_coeffs_out, display_profile->lutsize,
                             display_profile->nonlinearlut);

  // Sanitize values and ensure gamut-fitting
  // we reproduce the default behaviour of colorout, which is harsh gamut clipping
  color->red = CLAMP(RGB[0], 0.f, 1.f);
  color->green = CLAMP(RGB[1], 0.f, 1.f);
  color->blue = CLAMP(RGB[2], 0.f, 1.f);

  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
