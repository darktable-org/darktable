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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "tracing.h"
#include "testimg.h"

// epsilon for floating point comparison (1e-6 is approximately 20 EV below pure
// white):
#define E 1e-6f

Testimg *testimg_alloc(const int width, const int height)
{
  Testimg *ti = calloc(sizeof(Testimg), 1);
  ti->width = width;
  ti->height = height;
  ti->pixels = calloc((size_t)4 * width * height, sizeof(float));
  ti->name = "";
  return ti;
}

void testimg_free(Testimg *const ti)
{
  free(ti->pixels);
  free(ti);
}

void testimg_print_chan(const Testimg *const ti, int chan_idx)
{
  switch(chan_idx) {
    case 0: TR_DEBUG("RED"); break;
    case 1: TR_DEBUG("GREEN"); break;
    case 2: TR_DEBUG("BLUE"); break;
    case 3: TR_DEBUG("MASK"); break;
    default: return;
  }
  for_testimg_pixels_p_yx(ti)
  {
    printf(" %+.2e", p[chan_idx]);
    if(x == ti->width-1) printf("\n");
  }
}

void testimg_print_by_chan(const Testimg *const ti)
{
  TR_DEBUG("TEST IMAGE");
  TR_DEBUG("name=%s, width=%i, height=%i", ti->name, ti->width, ti->height);

  for(int c = 0; c < 4; c += 1) {
    testimg_print_chan(ti, c);
  }
}

void testimg_print_by_pixel(const Testimg *const ti)
{
  TR_DEBUG("TEST IMAGE");
  TR_DEBUG("name=%s, width=%i, height=%i", ti->name, ti->width, ti->height);

  for(int y = 0; y < ti->height; y += 1)
  {
    printf("y = %i\n", y);
    for(int c = 0; c < 4; c += 1)
    {
      for(int x = 0; x < ti->width; x += 1)
      {
        float *p = get_pixel(ti, x, y);
        printf(" %+.2e", p[c]);
      }
      printf("\n");
    }
  }
}

Testimg *testimg_to_log(Testimg *ti)
{
  for_testimg_pixels_p_yx(ti)
  {
    for(int c = 0; c < 3; c += 1)
    {
      p[c] = testimg_val_to_log(p[c]);
    }
  }
  return ti;
}

float testimg_val_to_log(const float val)
{
  return 1.0f - log2f(1.0f /  val) / TESTIMG_STD_DYN_RANGE_EV;
}

Testimg *testimg_to_exp(Testimg *ti)
{
  for_testimg_pixels_p_yx(ti)
  {
    for(int c = 0; c < 3; c += 1)
    {
      p[c] = testimg_val_to_exp(p[c]);
    }
  }
  return ti;
}

float testimg_val_to_exp(const float val)
{
  return exp2f(TESTIMG_STD_DYN_RANGE_EV * (val - 1.0f));
}

Testimg *testimg_gen_all_grey(const int width, const int height,
  const float value)
{
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "all grey";

  for_testimg_pixels_p_xy(ti)
  {
    p[0] = p[1] = p[2] = value;
  }
  return ti;
}

Testimg *testimg_gen_all_black(const int width, const int height)
{
  Testimg *ti = testimg_gen_all_grey(width, height, testimg_val_to_exp(0.0f));
  ti->name = "all black";
  return ti;
}

Testimg *testimg_gen_all_white(const int width, const int height)
{
  Testimg *ti = testimg_gen_all_grey(width, height, testimg_val_to_exp(1.0f));
  ti->name = "all white";
  return ti;
}

Testimg *testimg_gen_grey_space(const int width)
{
  const int height = 1;
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "grey space";

  for_testimg_pixels_p_xy(ti)
  {
    float val = (float)(x) / (float)(width-1);
    p[0] = p[1] = p[2] = testimg_val_to_exp(val);
  }
  return ti;
}

Testimg *testimg_gen_single_color_space(const int width, const int color_index)
{
  const int height = 1;
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "single color space";

  for_testimg_pixels_p_yx(ti)
  {
    float val = (float)(x) / (float)(width-1);
    p[color_index] = testimg_val_to_exp(val);
  }
  return ti;
}

Testimg *testimg_gen_three_color_space(const int width)
{
  const int height = 3;
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "three color space";

  for_testimg_pixels_p_yx(ti)
  {
    float val = (float)(x) / (float)(width-1);
    p[y] = testimg_val_to_exp(val);
  }
  return ti;
}

Testimg *testimg_gen_rgb_space(const int width)
{
  const int height = width * width;
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "rgb space";
  float *tmp = calloc(width, sizeof(float));

  for(int x = 0; x < width; x += 1)
  {
    float val = (float)(x) / (float)(width-1);
    tmp[x] = testimg_val_to_exp(val);
  }

  for_testimg_pixels_p_yx(ti)
  {
    p[0] = tmp[x];
    p[1] = tmp[y / width];
    p[2] = tmp[y % width];
  }
  free(tmp);
  return ti;
}

Testimg *testimg_gen_grey_max_dr()
{
  const int width = 10;
  const int height = 1;
  Testimg *ti = testimg_alloc(width, height);
  ti->name = "grey max dr";

  for_testimg_pixels_p_xy(ti)
  {
    switch(x)
    {
      case 0: p[0] = p[1] = p[2] = FLT_MIN; break;
      case 1: p[0] = p[1] = p[2] = 1e-20f; break;
      case 2: p[0] = p[1] = p[2] = 1e-10f; break;
      case 3: p[0] = p[1] = p[2] = 1e-5f; break;
      case 4: p[0] = p[1] = p[2] = 1e-1f; break;
      case 5: p[0] = p[1] = p[2] = 1.0f; break;
      case 6: p[0] = p[1] = p[2] = 1e5f; break;
      case 7: p[0] = p[1] = p[2] = 1e10f; break;
      case 8: p[0] = p[1] = p[2] = 1e20f; break;
      case 9: p[0] = p[1] = p[2] = FLT_MAX; break;
    }
  }
  return ti;
}

Testimg *testimg_gen_grey_max_dr_neg()
{
  Testimg *tmp = testimg_gen_grey_max_dr();
  Testimg *ti = testimg_alloc(tmp->width + 1, tmp->height);
  ti->name = "grey max dr neg";

  // copy values from testimg_grey_max_dr() in reverse order
  // and make them negative:
  for_testimg_pixels_p_xy(tmp)
  {
    float *p_tmp = get_pixel(tmp, tmp->width - x - 1, y);
    p[0] = p[1] = p[2] = -p_tmp[0];
  }
  // fill last value with 0.0:
  float *p = get_pixel(ti, ti->width - 1, 0);
  p[0] = p[1] = p[2] = -0.0f;

  testimg_free(tmp);
  return ti;
}

Testimg *testimg_gen_grey_with_rgb_clipping(const int width)
{
  Testimg *ti = testimg_alloc(width, 3);
  ti->name = "grey with rgb clipping";

  for_testimg_pixels_p_yx(ti)
  {
    float val = 0.9f + (float)(x) / (float)(width-1) / 10.0f;
    p[0] = testimg_val_to_exp(val);
    p[1] = testimg_val_to_exp(val);
    p[2] = testimg_val_to_exp(val);
    p[y] *= 1.25f;  // add some
  }
  return ti;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
