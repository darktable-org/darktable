/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

/*
  This API encapsulate the color picker behavior for IOP module. Providing
  4 routines (get_set, apply, reset and update, it will handle multiple
  color pickers in a module.

  A simpler version requires only apply to be passed and the picker widget when
  a single color picker is available in a module.
*/

#include <gtk/gtk.h>
#include "develop/imageop.h"

typedef enum _iop_color_picker_flags_t
{
  // at least one of point or area must be used
  DT_COLOR_PICKER_POINT = 1 << 0,
  DT_COLOR_PICKER_AREA = 1 << 1,
  DT_COLOR_PICKER_POINT_AREA = DT_COLOR_PICKER_POINT | DT_COLOR_PICKER_AREA
} dt_iop_color_picker_flags_t;

typedef struct dt_iop_color_picker_t
{
  // iop which contains this picker, or NULL if primary colorpicker
  dt_iop_module_t *module;
  dt_iop_color_picker_flags_t kind;
  /** requested colorspace for the color picker, valid options are:
   * IOP_CS_NONE: module colorspace
   * IOP_CS_LCH: for Lab modules
   * IOP_CS_HSL: for RGB modules
   */
  dt_iop_colorspace_type_t picker_cst;
  /** used to avoid recursion when a parameter is modified in the apply() */
  GtkWidget *colorpick;
  // positions are associated with the current picker widget: will set
  // the picker request for the primary picker when this picker is
  // activated, and will remember the most recent picker position
  float pick_pos[2];
  dt_boundingbox_t pick_box;
  gboolean changed;
} dt_iop_color_picker_t;


gboolean dt_iop_color_picker_is_visible(const dt_develop_t *dev);

//* reset current color picker if not keep-active or not keep */
void dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean keep);

/* sets the picker colorspace */
void dt_iop_color_picker_set_cst(dt_iop_module_t *module, const dt_iop_colorspace_type_t picker_cst);

/* returns the active picker colorspace (if any) */
dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module);

/* global init: link signal */
void dt_iop_color_picker_init();

/* global cleanup */
void dt_iop_color_picker_cleanup();

/* link color picker to widget */
GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_flags_t kind, GtkWidget *w);

/* link color picker to widget and initialize color picker color space with given value */
GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module, dt_iop_color_picker_flags_t kind, GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

