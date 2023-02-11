#pragma once
/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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

#include "develop/imageop.h"

// struct bundling all the auxiliary buffers
// required to fill the boundary of a full res pipeline
// with the coarse but complete roi preview pipeline
typedef struct local_laplacian_boundary_t
{
  int mode;                // 0-regular, 1-preview/collect, 2-full/read
  float *pad0;             // padded preview buffer, grey levels (allocated via dt_alloc_align)
  int wd;                  // preview width
  int ht;                  // preview height
  int pwd;                 // padded preview width
  int pht;                 // padded preview height
  const dt_iop_roi_t *roi; // roi of current view (pointing to pixelpipe roi)
  const dt_iop_roi_t *buf; // dimensions of full buffer
  float *output[30];       // output pyramid of preview pass (allocated via dt_alloc_align)
  int num_levels;          // number of levels in preview output pyramid
}
local_laplacian_boundary_t;

void local_laplacian_boundary_free(
    local_laplacian_boundary_t *b)
{
  dt_free_align(b->pad0);
  for(int l=0;l<b->num_levels;l++) dt_free_align(b->output[l]);
  memset(b, 0, sizeof(*b));
}

void local_laplacian_internal(
    const float *const input,   // input buffer in some Labx or yuvx format
    float *const out,           // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity,        // user param: increase clarity/local contrast
    const int use_sse2,         // switch on sse optimised version, if available
    // the following is just needed for clipped roi with boundary conditions from coarse buffer (can be 0)
    local_laplacian_boundary_t *b);

void local_laplacian(
    const float *const input,   // input buffer in some Labx or yuvx format
    float *const out,           // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity,        // user param: increase clarity/local contrast
    local_laplacian_boundary_t *b) // can be 0
{
  local_laplacian_internal(input, out, wd, ht, sigma, shadows, highlights, clarity, 0, b);
}

size_t local_laplacian_memory_use(const int width,      // width of input image
                                  const int height);    // height of input image


size_t local_laplacian_singlebuffer_size(const int width,       // width of input image
                                         const int height);     // height of input image


#if defined(__SSE2__)
void local_laplacian_sse2(
    const float *const input,   // input buffer in some Labx or yuvx format
    float *const out,           // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity,        // user param: increase clarity/local contrast
    local_laplacian_boundary_t *b) // can be 0
{
  local_laplacian_internal(input, out, wd, ht, sigma, shadows, highlights, clarity, 1, b);
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
