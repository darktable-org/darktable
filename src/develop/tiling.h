/*
    This file is part of darktable,
    copyright (c) 2011 ulrich pegelow.

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

#ifndef DT_DEVELOP_TILING_H
#define DT_DEVELOP_TILING_H

#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/pixelpipe.h"

typedef struct dt_develop_tiling_t
{
  /** memory requirement as a multiple of image buffer size */
  float factor;
  /** maximum requirement for temporary buffers as a multiple of image buffer size */
  float maxbuf;
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

int default_process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                              const int bpp);

int process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                      void *ovid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int bpp);

void default_process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                            void *ovid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                            const int bpp);

void process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                    void *ovid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int bpp);

void default_tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling);

void tiling_callback_blendop(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling);

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling);

int dt_tiling_piece_fits_host_memory(const size_t width, const size_t height, const unsigned bpp,
                                     const float factor, const size_t overhead);

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
