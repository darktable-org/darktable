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

#include "common/darktable.h"
#include "develop/blend.h"
#include "develop/masks.h"
#include "develop/pixelpipe.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <math.h>

static void test_inward_feathering_uses_the_selected_image_guide(void **state)
{
  darktable.num_openmp_threads = dt_get_num_procs();
  enum { WIDTH = 21, HEIGHT = 9, SIZE = WIDTH * HEIGHT };
  const dt_iop_roi_t roi = { .width = WIDTH, .height = HEIGHT, .scale = 1.0f };
  dt_develop_blend_params_t bp = { .blend_cst = DEVELOP_BLEND_CS_RGB_SCENE,
                                 .feather_version = 1 };
  float DT_ALIGNED_ARRAY input[4 * SIZE];
  float DT_ALIGNED_ARRAY output[4 * SIZE];
  float DT_ALIGNED_ARRAY mask_in[SIZE];
  float DT_ALIGNED_ARRAY mask_out[SIZE];

  for(int y = 0; y < HEIGHT; y++)
    for(int x = 0; x < WIDTH; x++)
    {
      const size_t k = y * WIDTH + x;
      for(int c = 0; c < 4; c++)
      {
        input[4 * k + c] = 0.2f;
        output[4 * k + c] = x < 6 ? 0.2f : 2.0f;
      }
      mask_in[k] = mask_out[k] = x >= 2
                                && x <= 18
                                && y >= 1
                                && y <= 7 ? 0.7f : 0.0f;
    }

  dt_dev_pixelpipe_iop_t piece = { .colors = 4, .iscale = 1.0f, .blendop_data = &bp,
                                 .blend_refine_guide_in = input,
                                 .blend_refine_guide_out = output,
                                 .blend_refine_roi_in = &roi };
  dt_masks_refinement_t r = { .enabled = DT_MASKS_REFINE_ELEMENT,
                             .feathering_radius = -3.0f,
                             .feathering_guide = DEVELOP_MASK_GUIDE_IN_AFTER_BLUR };
  dt_develop_blend_refine_form_mask(NULL, &piece, mask_in, &roi, &r);
  r.feathering_guide = DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR;
  dt_develop_blend_refine_form_mask(NULL, &piece, mask_out, &roi, &r);

  float difference = 0.0f;
  for(int y = 0; y < HEIGHT; y++)
    for(int x = 0; x < WIDTH; x++)
    {
      const size_t k = y * WIDTH + x;
      assert_true(mask_in[k] >= 0.0f && mask_in[k] <= 0.7f);
      assert_true(mask_out[k] >= 0.0f && mask_out[k] <= 0.7f);
      if(x <= 2
         || x >= 18
         || y <= 1
         || y >= 7)
      {
        assert_float_equal(mask_in[k], 0.0f, 1e-6f);
        assert_float_equal(mask_out[k], 0.0f, 1e-6f);
      }
      difference += fabsf(mask_in[k] - mask_out[k]);
    }
  assert_true(difference > 0.01f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_inward_feathering_uses_the_selected_image_guide),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
