#pragma once
/*
    This file is part of darktable,
    Copyright (C) 2020-2023 darktable developers.

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
  float scattering;	// scattering factor for patches (default 0 = densest possible)
  float scale;		// image scaling, affects scattering
  float luma;		// blend amount, L channel in Lab (set to 1.00 for RGB)
  float chroma;		// blend amount, a/b channels (set to 1.00 for RBG)
  float center_weight;  // weighting of central pixel in patch (<0 for no special handling; used by denoise[non-local])
  float sharpness;	// relative weight of central pixel (preserves detail), ignored if center_weight >= 0
  int patch_radius;	// radius of patches which are compared, 1..4
  int search_radius;	// radius around a pixel in which to compare patches (default = 7)
  int decimate;         // set to 1 to search only half the patches in the neighborhood (default = 0)
  const float* const norm; // array of four per-channel weight factors
  dt_dev_pixelpipe_type_t pipetype;
  int kernel_init;	// CL: initialization (runs once)
  int kernel_dist;	// CL: compute channel-normed squared pixel differences (runs for each patch)
  int kernel_horiz;	// CL: horizontal sum (runs for each patch)
  int kernel_vert;	// CL: vertical sum (runs for each patch)
  int kernel_accu;	// CL: add to output pixel (runs for each patch)
};
typedef struct dt_nlmeans_param_t dt_nlmeans_param_t;

void nlmeans_denoise(const float *const inbuf, float *const outbuf,
                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                     const dt_nlmeans_param_t *const params);

#ifdef HAVE_OPENCL
int nlmeans_denoise_cl(const dt_nlmeans_param_t *const params, const int devid,
                       cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *const roi_in);
int nlmeans_denoiseprofile_cl(const dt_nlmeans_param_t *const params, const int devid,
                              cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *const roi_in);
#endif /* HAVE_OPENCL */
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
