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

#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"

typedef struct dt_develop_tiling_t
{
  /** memory requirement as a multiple of image buffer size (on host/CPU) */
  float factor;
  /** memory requirement as a multiple of image buffer size (on GPU) */
  float factor_cl;
  /** maximum requirement for temporary buffers as a multiple of image buffer size (on host) */
  float maxbuf;
  /** maximum requirement for temporary buffers as a multiple of image buffer size (on GPU) */
  float maxbuf_cl;
  /** on-top memory requirement, with a size independent of input buffer */
  unsigned overhead;
  /** overlap needed between tiles (in pixels) */
  unsigned overlap;
  /** horizontal and vertical alignment requirement of upper left position
      of tiles. set to a value of 1 for no alignment, or 2 to account for
      Bayer pattern. */
  unsigned xalign;
  unsigned yalign;
} dt_develop_tiling_t;

int default_process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int bpp);

int process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                      const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                      const dt_iop_roi_t *const roi_out, const int bpp);

void default_process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                            const void *const ivoid, void *const ovid, const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out, const int bpp);

void process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                    const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out, const int bpp);

void default_tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling);

void tiling_callback_blendop(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling);

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling);

gboolean dt_tiling_piece_fits_host_memory(const size_t width, const size_t height, const unsigned bpp,
                                     const float factor, const size_t overhead);

float dt_tiling_estimate_cpumem(struct dt_develop_tiling_t *tiling, struct dt_dev_pixelpipe_iop_t *piece,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const int max_bpp);

#ifdef HAVE_OPENCL
float dt_tiling_estimate_clmem(struct dt_develop_tiling_t *tiling, struct dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const int max_bpp);
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
