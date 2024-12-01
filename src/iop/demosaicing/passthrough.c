/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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


static void passthrough_monochrome(float *out,
                                   const float *const in,
                                   const dt_iop_roi_t *const roi_in)
{
  const int width = roi_in->width;
  const int height = roi_in->height;

  DT_OMP_FOR(collapse(2))
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        out[(size_t)4 * ((size_t)j * width + i) + c] = in[(size_t)j * width + i];
      }
    }
  }
}

static void passthrough_color(float *out,
                              const float *const in,
                              const dt_iop_roi_t *const roi_in,
                              const uint32_t filters,
                              const uint8_t (*const xtrans)[6])
{
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(filters != 9u)
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const float val = in[(size_t)col + row * width];
        const size_t offset = (size_t)4 * ((size_t)row * width + col);
        const size_t ch = FC(row, col, filters);

        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
  else
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const float val = in[(size_t)col + row * width];
        const size_t offset = (size_t)4 * ((size_t)row * width + col);
        const size_t ch = FCxtrans(row, col, roi_in, xtrans);

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

