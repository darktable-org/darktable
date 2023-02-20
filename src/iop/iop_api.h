/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

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

#include "common/module_api.h"

#ifdef FULL_API_H

#include "common/introspection.h"

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <stdint.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_OPENCL
#include <CL/cl.h>
#endif

#if defined(__cplusplus) && !defined(INCLUDE_API_FROM_MODULE_H)
extern "C" {
#endif

struct dt_iop_module_so_t;
struct dt_iop_module_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_iop_roi_t;
struct dt_develop_tiling_t;
struct dt_iop_buffer_dsc_t;
struct _GtkWidget;

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

/* early definition of modules to do type checking */

#pragma GCC visibility push(default)

#endif // FULL_API_H

/** this initializes static, hardcoded presets for this module and is called only once per run of dt. */
OPTIONAL(void, init_presets, struct dt_iop_module_so_t *self);
/** called once per module, at startup. */
OPTIONAL(void, init_global, struct dt_iop_module_so_t *self);
/** called once per module, at shutdown. */
OPTIONAL(void, cleanup_global, struct dt_iop_module_so_t *self);

/** get name of the module, to be translated. */
REQUIRED(const char *, name, void);
/** get the alternative names or keywords of the module, to be translated. Separate variants by a pipe | */
DEFAULT(const char *, aliases, void);
/** get the default group this module belongs to. */
DEFAULT(int, default_group, void);
/** get the iop module flags. */
DEFAULT(int, flags, void);
/** get the deprecated message if needed, to be translated. */
DEFAULT(const char *, deprecated_msg, void);

/** get a descriptive text used for example in a tooltip in more modules */
DEFAULT(const char **, description, struct dt_iop_module_t *self);

DEFAULT(int, operation_tags, void);
DEFAULT(int, operation_tags_filter, void);

/** what do the iop want as an input? */
DEFAULT(void, input_format, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                             struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);
/** what will it output? */
DEFAULT(void, output_format, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                              struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);

/** what default colorspace this iop use? */
REQUIRED(int, default_colorspace, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                                  struct dt_dev_pixelpipe_iop_t *piece);
/** what input colorspace it expects? */
DEFAULT(int, input_colorspace, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                                struct dt_dev_pixelpipe_iop_t *piece);
/** what will it output? */
DEFAULT(int, output_colorspace, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                                 struct dt_dev_pixelpipe_iop_t *piece);
/** what colorspace the blend module operates with? */
DEFAULT(int, blend_colorspace, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                                struct dt_dev_pixelpipe_iop_t *piece);

/** report back info for tiling: memory usage and overlap. Memory usage: factor * input_size + overhead */
DEFAULT(void, tiling_callback, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                                struct dt_develop_tiling_t *tiling);

/** callback methods for gui. */
/** synch gtk interface with gui params, if necessary. */
OPTIONAL(void, gui_update, struct dt_iop_module_t *self);
/** reset ui to defaults */
OPTIONAL(void, gui_reset, struct dt_iop_module_t *self);
/** construct widget. */
OPTIONAL(void, gui_init, struct dt_iop_module_t *self);
/** apply color picker results */
OPTIONAL(void, color_picker_apply, struct dt_iop_module_t *self, struct _GtkWidget *picker, struct dt_dev_pixelpipe_iop_t *piece);
/** called by standard widget callbacks after value changed */
OPTIONAL(void, gui_changed, struct dt_iop_module_t *self, GtkWidget *widget, void *previous);
/** destroy widget. */
DEFAULT(void, gui_cleanup, struct dt_iop_module_t *self);
/** optional method called after darkroom expose. */
OPTIONAL(void, gui_post_expose, struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                                int32_t pointerx, int32_t pointery);
/** optional callback to be notified if the module acquires gui focus/loses it. */
OPTIONAL(void, gui_focus, struct dt_iop_module_t *self, gboolean in);

/** Key accelerator registration callbacks */
OPTIONAL(GSList *, mouse_actions, struct dt_iop_module_t *self);

/** optional event callbacks */
OPTIONAL(int, mouse_leave, struct dt_iop_module_t *self);
OPTIONAL(int, mouse_moved, struct dt_iop_module_t *self, double x, double y, double pressure, int which);
OPTIONAL(int, button_released, struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
OPTIONAL(int, button_pressed, struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                              uint32_t state);

OPTIONAL(int, scrolled, struct dt_iop_module_t *self, double x, double y, int up, uint32_t state);
OPTIONAL(void, configure, struct dt_iop_module_t *self, int width, int height);

OPTIONAL(void, init, struct dt_iop_module_t *self); // this MUST set params_size!
DEFAULT(void, cleanup, struct dt_iop_module_t *self);

/** this inits the piece of the pipe, allocing piece->data as necessary. */
DEFAULT(void, init_pipe, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);
/** this resets the params to factory defaults. used at the beginning of each history synch. */
/** this commits (a mutex will be locked to synch pipe/gui) the given history params to the pixelpipe piece.
 */
DEFAULT(void, commit_params, struct dt_iop_module_t *self, dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe,
                              struct dt_dev_pixelpipe_iop_t *piece);
/** this is the chance to update default parameters, after the full raw is loaded. */
OPTIONAL(void, reload_defaults, struct dt_iop_module_t *self);
/** called after the image has changed in darkroom */
OPTIONAL(void, change_image, struct dt_iop_module_t *self);

/** this destroys all resources needed by the piece of the pixelpipe. */
DEFAULT(void, cleanup_pipe, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                             struct dt_dev_pixelpipe_iop_t *piece);
OPTIONAL(void, modify_roi_in, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
OPTIONAL(void, modify_roi_out, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
OPTIONAL(int, legacy_params, struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                             void *new_params, const int new_version);
// allow to select a shape inside an iop
OPTIONAL(void, masks_selection_changed, struct dt_iop_module_t *self, const int form_selected_id);

/** this is the temp homebrew callback to operations.
  * x,y, and scale are just given for orientation in the framebuffer. i and o are
  * scaled to the same size width*height and contain a max of 3 floats. other color
  * formats may be filled by this callback, if the pipeline can handle it. */
/** the simplest variant of process(). you can only use OpenMP SIMD here, no intrinsics */
/** must be provided by each IOP. */
REQUIRED(void, process, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                        void *const o, const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);
/** a tiling variant of process(). */
DEFAULT(void, process_tiling, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                               void *const o, const struct dt_iop_roi_t *const roi_in,
                               const struct dt_iop_roi_t *const roi_out, const int bpp);

#if defined(__SSE__)
/** a variant process(), that can contain SSE2 intrinsics. */
/** can be provided by each IOP. */
OPTIONAL(void, process_sse2, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                             void *const o, const struct dt_iop_roi_t *const roi_in,
                             const struct dt_iop_roi_t *const roi_out);
#endif

#ifdef HAVE_OPENCL
/** the opencl equivalent of process(). */
OPTIONAL(int, process_cl, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                          cl_mem dev_out, const struct dt_iop_roi_t *const roi_in,
                          const struct dt_iop_roi_t *const roi_out);
/** a tiling variant of process_cl(). */
OPTIONAL(int, process_tiling_cl, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                                 void *const o, const struct dt_iop_roi_t *const roi_in,
                                 const struct dt_iop_roi_t *const roi_out, const int bpp);
#endif

/** this functions are used for distort iop
 * points is an array of float {x1,y1,x2,y2,...}
 * size is 2*points_count */
/** points before the iop is applied => point after processed */
DEFAULT(int, distort_transform, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                                 size_t points_count);
/** reverse points after the iop is applied => point before process */
DEFAULT(int, distort_backtransform, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                                     size_t points_count);

OPTIONAL(void, distort_mask, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                             float *const out, const struct dt_iop_roi_t *const roi_in, const struct dt_iop_roi_t *const roi_out);

// introspection related callbacks, will be auto-implemented if DT_MODULE_INTROSPECTION() is used,
OPTIONAL(int, introspection_init, struct dt_iop_module_so_t *self, int api_version);
DEFAULT(dt_introspection_t *, get_introspection, void);
DEFAULT(dt_introspection_field_t *, get_introspection_linear, void);
DEFAULT(void *, get_p, const void *param, const char *name);
DEFAULT(dt_introspection_field_t *, get_f, const char *name);

// optional preference entry to add at the bottom of the preset menu
OPTIONAL(void, set_preferences, void *menu, struct dt_iop_module_t *self);

#ifdef FULL_API_H

#pragma GCC visibility pop

#if defined(__cplusplus) && !defined(INCLUDE_API_FROM_MODULE_H)
} // extern "C"
#endif

#endif // FULL_API_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

