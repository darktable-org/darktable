/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.
    copyright (c) 2016 Roman Lebedev.

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

#ifdef __cplusplus
extern "C" {
#endif

#include "common/introspection.h"

#include <cairo/cairo.h>
#include <glib.h>
#include <stdint.h>

#ifdef HAVE_OPENCL
#include <CL/cl.h>
#endif

struct dt_iop_module_so_t;
struct dt_iop_module_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_iop_roi_t;
struct dt_develop_tiling_t;
struct dt_iop_buffer_dsc_t;

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

/* early definition of modules to do type checking */

// !!! MUST BE KEPT IN SYNC WITH dt_iop_module_so_t and dt_iop_module_t defined in src/develop/imageop.h !!!

#pragma GCC visibility push(default)

/** this initializes static, hardcoded presets for this module and is called only once per run of dt. */
void init_presets(struct dt_iop_module_so_t *self);
/** called once per module, at startup. */
void init_global(struct dt_iop_module_so_t *self);
/** called once per module, at shutdown. */
void cleanup_global(struct dt_iop_module_so_t *self);

/** version of the parameters in the database. */
int version();
/** get name of the module, to be translated. */
const char *name();
/** get the groups this module belongs to. */
int groups();
/** get the iop module flags. */
int flags();

/** get a descriptive text used for example in a tooltip in more modules */
const char *description();

int operation_tags();
int operation_tags_filter();

/** what do the iop want as an input? */
void input_format(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                  struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);
/** what will it output? */
void output_format(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);

/** report back info for tiling: memory usage and overlap. Memory usage: factor * input_size + overhead */
void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling);

/** callback methods for gui. */
/** synch gtk interface with gui params, if necessary. */
void gui_update(struct dt_iop_module_t *self);
/** reset ui to defaults */
void gui_reset(struct dt_iop_module_t *self);
/** construct widget. */
void gui_init(struct dt_iop_module_t *self);
/** destroy widget. */
void gui_cleanup(struct dt_iop_module_t *self);
/** optional method called after darkroom expose. */
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery);
/** optional callback to be notified if the module acquires gui focus/loses it. */
void gui_focus(struct dt_iop_module_t *self, gboolean in);

/** Optional callback for keyboard accelerators */
void init_key_accels(struct dt_iop_module_so_t *so);
void original_init_key_accels(struct dt_iop_module_so_t *so);
/** Key accelerator registration callbacks */
void connect_key_accels(struct dt_iop_module_t *self);
void original_connect_key_accels(struct dt_iop_module_t *self);
void disconnect_key_accels(struct dt_iop_module_t *self);

/** optional event callbacks */
int mouse_leave(struct dt_iop_module_t *self);
int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which);
int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state);
int key_pressed(struct dt_iop_module_t *self, uint16_t which);

int scrolled(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state);
void configure(struct dt_iop_module_t *self, int width, int height);

void init(struct dt_iop_module_t *self); // this MUST set params_size!
void original_init(struct dt_iop_module_t *self);
void cleanup(struct dt_iop_module_t *self);

/** this inits the piece of the pipe, allocing piece->data as necessary. */
void init_pipe(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
               struct dt_dev_pixelpipe_iop_t *piece);
/** this resets the params to factory defaults. used at the beginning of each history synch. */
/** this commits (a mutex will be locked to synch pipe/gui) the given history params to the pixelpipe piece.
 */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece);
/** this is the chance to update default parameters, after the full raw is loaded. */
void reload_defaults(struct dt_iop_module_t *self);

/** this destroys all resources needed by the piece of the pixelpipe. */
void cleanup_pipe(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                  struct dt_dev_pixelpipe_iop_t *piece);
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                    struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
int legacy_params(struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version);
// allow to select a shape inside an iop
void masks_selection_changed(struct dt_iop_module_t *self, const int form_selected_id);

/** this is the temp homebrew callback to operations.
  * x,y, and scale are just given for orientation in the framebuffer. i and o are
  * scaled to the same size width*height and contain a max of 3 floats. other color
  * formats may be filled by this callback, if the pipeline can handle it. */
/** the simplest variant of process(). you can only use OpenMP SIMD here, no intrinsics */
/** must be provided by each IOP. */
void process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
             void *const o, const struct dt_iop_roi_t *const roi_in,
             const struct dt_iop_roi_t *const roi_out);
/** a tiling variant of process(). */
void process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                    void *const o, const struct dt_iop_roi_t *const roi_in,
                    const struct dt_iop_roi_t *const roi_out, const int bpp);

#if defined(__SSE__)
/** a variant process(), that can contain SSE2 intrinsics. */
/** can be provided by each IOP. */
void process_sse2(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                  void *const o, const struct dt_iop_roi_t *const roi_in,
                  const struct dt_iop_roi_t *const roi_out);
#endif

#ifdef HAVE_OPENCL
/** the opencl equivalent of process(). */
int process_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
               cl_mem dev_out, const struct dt_iop_roi_t *const roi_in,
               const struct dt_iop_roi_t *const roi_out);
/** a tiling variant of process_cl(). */
int process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                      void *const o, const struct dt_iop_roi_t *const roi_in,
                      const struct dt_iop_roi_t *const roi_out, const int bpp);
#endif

/** this functions are used for distort iop
 * points is an array of float {x1,y1,x2,y2,...}
 * size is 2*points_count */
/** points before the iop is applied => point after processed */
int distort_transform(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                      size_t points_count);
/** reverse points after the iop is applied => point before process */
int distort_backtransform(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count);


// introspection related callbacks, will be auto-implemented if DT_MODULE_INTROSPECTION() is used,
int introspection_init(struct dt_iop_module_so_t *self, int api_version);
dt_introspection_t *get_introspection();
dt_introspection_field_t *get_introspection_linear();
void *get_p(const void *param, const char *name);
dt_introspection_field_t *get_f(const char *name);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
