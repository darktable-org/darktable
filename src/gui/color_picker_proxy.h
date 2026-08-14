/*
    This file is part of darktable,
    Copyright (C) 2018-2025 darktable developers.

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
#include "common/darktable.h"
#include "develop/imageop.h"

typedef enum _iop_color_picker_flags_t
{
  // at least one of point or area must be used
  DT_COLOR_PICKER_POINT = 1 << 0,
  DT_COLOR_PICKER_AREA = 1 << 1,
  DT_COLOR_PICKER_POINT_AREA = DT_COLOR_PICKER_POINT | DT_COLOR_PICKER_AREA,
  // only works with 4-channel images
  DT_COLOR_PICKER_DENOISE = 1 << 2,
  // all pickers sample input, only ones with this flag set sample output
  DT_COLOR_PICKER_IO = 1 << 3
} dt_iop_color_picker_flags_t;

typedef struct dt_iop_color_picker_t
{
  // iop which contains this picker, or NULL if primary colorpicker
  dt_iop_module_t *module;
  dt_iop_color_picker_flags_t flags;
  /** requested colorspace for the color picker, valid options are:
   * IOP_CS_NONE: module colorspace
   * IOP_CS_LCH: for Lab modules
   * IOP_CS_HSL: for RGB modules
   */
  dt_iop_colorspace_type_t picker_cst;
  /* if we define via dt_color_picker_new_with_cst() we don't want the picker callbacks
     to modify it's picker_cst
  */
  gboolean fixed_cst;
  /** used to avoid recursion when a parameter is modified in the apply() */
  GtkWidget *colorpick;
  // positions are associated with the current picker widget: will set
  // the picker request for the primary picker when this picker is
  // activated, and will remember the most recent picker position
  dt_pickerpoint_t pick_pos;
  dt_pickerbox_t pick_box;
  gboolean initialized;
  gboolean changed;
} dt_iop_color_picker_t;


gboolean dt_iop_color_picker_is_visible(const dt_develop_t *dev);

/* g_object data key on the picker widget: the owning dt_iop_color_picker_t */
#define DT_COLOR_PICKER_INSTANCE_KEY "dt-color-picker-instance"

/* action definition for standalone picker toggle buttons (see color_picker_proxy.c) */
extern const struct dt_action_def_t dt_action_def_color_picker;

/* Shared activation entry for standalone picker toggle buttons, invoked by
 * real clicks (CAPTURE-phase gesture), shortcuts (dt_action_def_color_picker)
 * and programmatic activation (temperature.c "spot" preset, colorpicker lib
 * sample copy).  Same semantics as the toggle action: TOGGLE_CTRL/ON_CTRL
 * switch to area mode, TOGGLE_RIGHT/ON_RIGHT select area mode as well. */
float dt_iop_color_picker_toggle(GtkWidget *button,
                                 const dt_action_effect_t effect,
                                 const float move_size);

//* reset current color picker if not keep-active or not keep */
void dt_iop_color_picker_reset(dt_iop_module_t *module,
                               const gboolean keep);

/* sets the picker colorspace */
void dt_iop_color_picker_set_cst(dt_iop_module_t *module,
                                 const dt_iop_colorspace_type_t picker_cst);

/* returns the active picker colorspace (if any) */
dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module);

/* global init: link signal */
void dt_iop_color_picker_init();

/* global cleanup */
void dt_iop_color_picker_cleanup();

/* link color picker to widget */
GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_flags_t flags, GtkWidget *w);

/* link color picker to widget and initialize color picker color space with given value */
GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module,
                                        const dt_iop_color_picker_flags_t flags,
                                        GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst);

gboolean dt_iop_color_picker_is_active(GtkWidget *w);

/* toggle a standalone picker toggle button programmatically; routes through
 * the same shared entry as real clicks and shortcuts
 * (dt_action_def_color_picker -> dt_iop_color_picker_toggle) */
void dt_color_picker_toggle(GtkWidget *target);

/* activate a standalone picker toggle button like a real click, with the
 * right button selecting area mode and the left one point mode */
void dt_color_picker_click(GtkWidget *target, const gboolean right);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
