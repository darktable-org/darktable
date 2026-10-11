/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include <stdint.h>
#include "common/opencl.h"
#include "image.h"

G_BEGIN_DECLS

typedef struct dt_dng_gain_map_t
{
  uint32_t top;
  uint32_t left;
  uint32_t bottom;
  uint32_t right;
  uint32_t plane;
  uint32_t planes;
  uint32_t row_pitch;
  uint32_t col_pitch;
  uint32_t map_points_v;
  uint32_t map_points_h;
  double map_spacing_v;
  double map_spacing_h;
  double map_origin_v;
  double map_origin_h;
  uint32_t map_planes;
  float map_gain[];
} dt_dng_gain_map_t;

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t size, dt_image_t *img);
void dt_dng_opcode_process_opcode_list_3(uint8_t *buf, uint32_t size, dt_image_t *img);

struct dt_iop_roi_t;

/* Return the OpcodeList3 GainMap of img if the list holds exactly one map and it
   is of a form we support, NULL otherwise. OpcodeList3 is applied "just after
   the image has been demosaiced" (DNG 1.7.1, tag 51022), so the data is always
   three colour planes and no CFA layout is involved.

   Callers are demosaic for CFA raws and rawprepare for linear DNGs, which are
   mutually exclusive on pipe->dsc.filters. */
const dt_dng_gain_map_t *dt_dng_gain_map_opcode3(const dt_image_t *img);

/* Multiply the GainMap into 4-channel float data. in and out may be the same
   buffer.

   The caller supplies the geometry explicitly rather than passing a roi,
   because the two callers do not derive it the same way. x0/y0 are the full
   resolution raw sensor coordinates of output pixel (0,0), including whatever
   crop rawprepare applied, and step is how many raw pixels one output pixel
   spans. Deriving these from roi_out alone is wrong in demosaic: the resample
   that produces its output ignores roi shifts (see the comment on
   dt_iop_clip_and_zoom_roi), so the buffer origin is roi_in's, which differs
   from roi_out->x / roi_out->scale by the integer truncation in
   demosaic's modify_roi_in. */
void dt_dng_gain_map_apply(const dt_dng_gain_map_t *gm,
                           const dt_image_t *img,
                           const float *const in,
                           float *const out,
                           const int width,
                           const int height,
                           const float x0,
                           const float y0,
                           const float step);

#ifdef HAVE_OPENCL
typedef struct dt_dng_gain_map_cl_global_t
{
  int kernel_dng_gain_map;
} dt_dng_gain_map_cl_global_t;

dt_dng_gain_map_cl_global_t *dt_dng_gain_map_init_cl_global(void);
void dt_dng_gain_map_free_cl_global(dt_dng_gain_map_cl_global_t *g);

/* OpenCL counterpart of dt_dng_gain_map_apply(). Returns a CL error code so the
   caller can let the pixelpipe fall back to the CPU rather than silently
   dropping the correction. */
int dt_dng_gain_map_apply_cl(const int devid,
                             const dt_dng_gain_map_t *gm,
                             const dt_image_t *img,
                             cl_mem dev_in,
                             cl_mem dev_out,
                             const int width,
                             const int height,
                             const float x0,
                             const float y0,
                             const float step);
#endif

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
