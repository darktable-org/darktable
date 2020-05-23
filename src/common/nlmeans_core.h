#pragma once
/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include "iop/iop_api.h"

struct dt_nlmeans_param_t
{
  float sharpness;	// relative weight of central pixel (preserves detail)
  float luma;		// blend amount, L channel in Lab (set to 1.00 for RGB)
  float chroma;		// blend amount, a/b channels (set to 1.00 for RBG)
  float scattering;	// scattering factor for patches (default 0 = densest possible)
  float scale;		// image scaling, affects scattering
<<<<<<< HEAD
<<<<<<< HEAD
  float center_weight;  // weighting of central pixel in patch (<0 for no special handling; used by denoise[non-local])
=======
>>>>>>> New more-scaleable implementation of non-local means
=======
  float center_weight;  // weighting of central pixel in patch (<0 for no special handling; used by denoise[non-local])
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
  int patch_radius;	// radius of patches which are compared, 1..4
  int search_radius;	// radius around a pixel in which to compare patches (default = 7)
  int decimate;         // set to 1 to search only half the patches in the neighborhood (default = 0)
  const float* const norm; // array of four per-channel weight factors
};
typedef struct dt_nlmeans_param_t dt_nlmeans_param_t;

void nlmeans_denoise(const float *const inbuf, float *const outbuf,
                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                     const dt_nlmeans_param_t *const params);
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> add SSE implementation of nlmeans_denoise

void nlmeans_denoise_sse2(const float *const inbuf, float *const outbuf,
                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                          const dt_nlmeans_param_t *const params);
<<<<<<< HEAD
=======
>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> add SSE implementation of nlmeans_denoise
