/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// the replay harness of the harvest checks that render: it builds a
// dt_develop_t, a module, a pixelpipe and a piece around a harvested edit,
// over the probe image, and renders the mask through
// dt_develop_blend_process(). Checks differ in what they do to the mask
// between renders, not in how they render it. For the harvest tools in
// src/develop/masks/ only, never the pipeline

#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe.h"

#include <glib.h>

G_BEGIN_DECLS

// masks are normalized, so the replay renders at a bounded size, which keeps
// every shape's proportions and every channel's behavior while keeping
// thousands of renders affordable
#define VERIFY_MAX_EDGE 512

typedef struct
{
  dt_develop_t dev;
  dt_iop_module_t module;
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  gboolean module_loaded;
  gboolean dev_mutex_ready;
  dt_iop_roi_t roi;
  float *probe;
  // the module's output: the probe with a synthetic effect applied
  // (_make_module_output). The blend mixes it with `probe` by the mask, so
  // the image responds to the mask, and the blendif output channels have
  // something of their own to select on
  float *modout;
  float *out;

  // the upstream module a raster edit reads its mask from, present only for
  // raster edits (see _attach_raster_source)
  dt_iop_module_t source_module;
  dt_dev_pixelpipe_iop_t source_piece;
  gboolean source_loaded;

  // the OpenCL device to replay the GPU blend on, or -1 without one (the CPU
  // comparison stands on its own)
  int devid;

  // darktable.develop before the replay took it over, restored on cleanup
  // (see dt_masks_verify_replay_init)
  dt_develop_t *saved_develop;

  // the canvas editing state a shape's modify_property() reads; dev.form_gui
  // points at it. See dt_masks_verify_replay_init for why it cannot simply be NULL.
  dt_masks_form_gui_t form_gui;
} replay_t;

/** the size the replay renders an image of `full_width` x `full_height` at:
    at most VERIFY_MAX_EDGE, with the image's aspect, since masks are
    normalized and a wrong aspect would distort every shape */
void dt_masks_verify_replay_size(const int full_width,
                                 const int full_height,
                                 int *width,
                                 int *height);

/** build a replay around one harvested edit. NULL on success, else a static
    string naming what could not be set up */
const char *dt_masks_verify_replay_init(replay_t *r,
                                        const char *operation,
                                        const dt_develop_blend_params_t *bp,
                                        GList *forms,
                                        const int full_width,
                                        const int full_height,
                                        const int width,
                                        const int height);

/** render the mask of the current blend params and forms into a copy the
    caller owns; NULL if the blend published nothing. `image`, which may be
    NULL, receives the rendered RGBA image the same way */
float *dt_masks_verify_render_mask(replay_t *r, float **image);

/** free everything the replay allocated */
void dt_masks_verify_replay_cleanup(replay_t *r);

/** the largest absolute difference between two buffers */
double dt_masks_verify_max_abs_diff(const float *a, const float *b, const size_t n);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
