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

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "iop/rawdenoise.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void _check_mosaic(const int width, const int height, const gboolean separate_colors)
{
  static const uint8_t base[6][6] = {
    { 1, 1, 0, 1, 1, 2 },
    { 1, 1, 2, 1, 1, 0 },
    { 2, 0, 1, 0, 2, 1 },
    { 1, 1, 2, 1, 1, 0 },
    { 1, 1, 0, 1, 1, 2 },
    { 0, 2, 1, 2, 0, 1 }
  };
  static const float values[3] = { 0.04f, 0.09f, 0.16f };
  const gboolean small = width < 6 || height < 6;
  const dt_iop_roi_t roi = { .width = width, .height = height };
  dt_iop_rawdenoise_data_t data = { .threshold = 0.05f };
  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      data.force[ch][k] = 0.5f;

  float *const in = dt_alloc_align_float((size_t)width * height);
  float *const out = dt_alloc_align_float((size_t)width * height);
  assert_non_null(in);
  assert_non_null(out);
  for(int sr = 0; sr < 6; sr++)
    for(int sc = 0; sc < 6; sc++)
    {
      uint8_t xtrans[6][6];
      for(int row = 0; row < 6; row++)
        for(int col = 0; col < 6; col++)
          xtrans[row][col] = base[(row + sr) % 6][(col + sc) % 6];

      for(int row = 0; row < height; row++)
        for(int col = 0; col < width; col++)
        {
          const size_t k = (size_t)row * width + col;
          in[k] = small ? (float)(k % 17) / 17.0f
                        : values[separate_colors ? FCNxtrans(row, col, xtrans) : 0];
          out[k] = NAN;
        }

      wavelet_denoise_xtrans(in, out, &roi, &data, xtrans);

      // five wavelet bands spread the separate row-1 side-edge defect by at most 31 columns
      const int margin = separate_colors ? (1 << DT_IOP_RAWDENOISE_BANDS) : 0;
      for(int row = 0; row < height; row++)
        for(int col = margin; col < width - margin; col++)
        {
          const size_t k = (size_t)row * width + col;
          assert_true(isfinite(out[k]));
          assert_float_equal(out[k], in[k], 1e-6f);
        }

      if(small)
        assert_memory_equal(out, in, (size_t)width * height * sizeof(float));
    }
  dt_free_align(out);
  dt_free_align(in);
}

static void _test_xtrans_constant_borders(void **state)
{
  for(int height = 6; height < 12; height++)
    for(int width = 6; width < 12; width++)
      _check_mosaic(width, height, FALSE);

  for(int height = 96; height < 102; height++)
    _check_mosaic(97, height, TRUE);
}

static void _test_xtrans_small_region_passthrough(void **state)
{
  for(int size = 1; size < 6; size++)
  {
    _check_mosaic(size, 12, FALSE);
    _check_mosaic(12, size, FALSE);
  }
}

int main(int argc, char *argv[])
{
  darktable.num_openmp_threads = 1;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_xtrans_constant_borders),
    cmocka_unit_test(_test_xtrans_small_region_passthrough)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
