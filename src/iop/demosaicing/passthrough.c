/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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


/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void passthrough_monochrome(
        float *out,
        const float *const in,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in)
{
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, roi_out, roi_in) \
  shared(out) \
  schedule(static) collapse(2)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        out[(size_t)4 * ((size_t)j * roi_out->width + i) + c]
            = in[(size_t)((size_t)j + roi_out->y) * roi_in->width + i + roi_out->x];
      }
    }
  }
}

static void passthrough_color(
        float *out,
        const float *const in,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6])
{
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);

  if(filters != 9u)
  {
    #ifdef _OPENMP
      #pragma omp parallel for default(none) \
      dt_omp_firstprivate(in, roi_out, roi_in, filters) \
      shared(out) \
      schedule(static) \
      collapse(2)
    #endif

    for(int row = 0; row < (roi_out->height); row++)
    {
      for(int col = 0; col < (roi_out->width); col++)
      {
        const float val = in[col + roi_out->x + ((row + roi_out->y) * roi_in->width)];
        const uint32_t offset = (size_t)4 * ((size_t)row * roi_out->width + col);
        const uint32_t ch = FC(row + roi_out->y, col + roi_out->x, filters);

        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
  else
  {
    #ifdef _OPENMP
      #pragma omp parallel for default(none) \
      dt_omp_firstprivate(in, roi_out, roi_in, xtrans) \
      shared(out) \
      schedule(static) \
      collapse(2)
    #endif

    for(int row = 0; row < (roi_out->height); row++)
    {
      for(int col = 0; col < (roi_out->width); col++)
      {
        const float val = in[col + roi_out->x + ((row + roi_out->y) * roi_in->width)];
        const uint32_t offset = (size_t)4 * ((size_t)row * roi_out->width + col);
        const uint32_t ch = FCxtrans(row, col, roi_in, xtrans);

        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

