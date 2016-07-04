/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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

#ifndef DT_COMMON_BILATERAL_H
#define DT_COMMON_BILATERAL_H

#include <stddef.h> // for size_t

typedef struct dt_bilateral_t
{
  size_t size_x, size_y, size_z;
  int width, height;
  float sigma_s, sigma_r;
  float *buf;
} dt_bilateral_t;

size_t dt_bilateral_memory_use(const int width,      // width of input image
                               const int height,     // height of input image
                               const float sigma_s,  // spatial sigma (blur pixel coords)
                               const float sigma_r); // range sigma (blur luma values)

size_t dt_bilateral_memory_use2(const int width,      // width of input image
                                const int height,     // height of input image
                                const float sigma_s,  // spatial sigma (blur pixel coords)
                                const float sigma_r); // range sigma (blur luma values)

size_t dt_bilateral_singlebuffer_size(const int width,      // width of input image
                                      const int height,     // height of input image
                                      const float sigma_s,  // spatial sigma (blur pixel coords)
                                      const float sigma_r); // range sigma (blur luma values)

size_t dt_bilateral_singlebuffer_size2(const int width,      // width of input image
                                       const int height,     // height of input image
                                       const float sigma_s,  // spatial sigma (blur pixel coords)
                                       const float sigma_r); // range sigma (blur luma values)

dt_bilateral_t *dt_bilateral_init(const int width,      // width of input image
                                  const int height,     // height of input image
                                  const float sigma_s,  // spatial sigma (blur pixel coords)
                                  const float sigma_r); // range sigma (blur luma values)

void dt_bilateral_splat(dt_bilateral_t *b, const float *const in);

void dt_bilateral_blur(dt_bilateral_t *b);

void dt_bilateral_slice(const dt_bilateral_t *const b, const float *const in, float *out, const float detail);

void dt_bilateral_slice_to_output(const dt_bilateral_t *const b, const float *const in, float *out,
                                  const float detail);

void dt_bilateral_free(dt_bilateral_t *b);

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
