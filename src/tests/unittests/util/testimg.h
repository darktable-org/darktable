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
/*
 * Test image generation, access, printing etc. to be used for unit testing with
 * cmocka.
 *
 * Please see ../README.md for more detailed documentation.
 */

typedef struct Testimg
{
  int width;
  int height;
  float *pixels;  // [0]=red, [1]=green, [2]=blue, [3]=misc/mask
  const char* name;
} Testimg;


/*
 * Creation/deletion
 */

// standard dynamic range for test images in EV:
#define TESTIMG_STD_DYN_RANGE_EV 15

// standard width/height for test images:
#define TESTIMG_STD_WIDTH (TESTIMG_STD_DYN_RANGE_EV + 1)
#define TESTIMG_STD_HEIGHT (TESTIMG_STD_DYN_RANGE_EV + 1)

// allocate an empty test image:
Testimg *testimg_alloc(const int width, const int height);

// free test image after usage:
void testimg_free(Testimg *const ti);


/*
 * Access
 */

// access pixel (x -> width, y -> height):
inline float* get_pixel(const Testimg *const ti, const int x, const int y)
{
  // y * width + x, so pixel at index 2 is top row, 2nd from left:
  return ti->pixels + (y * ti->width + x) * 4;
}

// iterate over test images (x in outer loop):
#define for_testimg_pixels_p_xy(ti) \
  for (int x=0, y=0; x<ti->width; x+=1, y=0)\
    for (float *p=get_pixel(ti, x, y); y<ti->height; y+=1, p=get_pixel(ti, x, y))

// iterate over test images (y in outer loop):
#define for_testimg_pixels_p_yx(ti) \
  for (int y=0, x=0; y<ti->height; y+=1, x=0)\
    for (float *p=get_pixel(ti, x, y); x<ti->width; x+=1, p=get_pixel(ti, x, y))


/*
 * Printing
 */

// print a color channel of a test image:
void testimg_print_chan(const Testimg *const ti, const int chan_idx);

// print a whole image, each color channel separately:
void testimg_print_by_chan(const Testimg *const ti);

// print a whole image, each pixel separately:
void testimg_print_by_pixel(const Testimg *const ti);

// default print:
#define testimg_print testimg_print_by_pixel


/*
 * Conversion
 */

// convert a test image to log-RGB with fixed white point of 1.0f and dynamic
// range of TESTIMG_STD_DYN_RANGE_EV:
Testimg *testimg_to_log(Testimg *ti);

// convert a single value to log-RGB with fixed white point of 1.0f and dynamic
// range of TESTIMG_STD_DYN_RANGE_EV:
float testimg_val_to_log(const float val);

// convert a test image to exp-RGB (i.e. a test image in log-RGB back to
// linear-RGB) with fixed white point 1.0f and dynamic range
// TESTIMG_STD_DYN_RANGE_EV:
Testimg *testimg_to_exp(Testimg *ti);

// convert a single value to exp-RGB (i.e. a value in log-RGB back to
// linear-RGB) with fixed white point 1.0f and dynamic range
// TESTIMG_STD_DYN_RANGE_EV:
float testimg_val_to_exp(const float val);


/*
 * Constant color image generation
 */

// create an image of given size with constant grey color:
Testimg *testimg_gen_all_grey(const int width, const int height,
  const float value);

// create a purely black image:
Testimg *testimg_gen_all_black(const int width, const int height);

// create a purely white image:
Testimg *testimg_gen_all_white(const int width, const int height);


/*
 * Full color space image generation
 */

// create a grey gradient from black (left) to white (right) with given width
// and fixed height=1:
Testimg *testimg_gen_grey_space(const int width);

// create a gradient of 1 color from black (left) to white (right) with given
// width and height=1 (0=red, 1=green, 2=blue):
Testimg *testimg_gen_single_color_space(const int width, const int color_index);

// create a gradient of 3 colors from black (left) to white (right) with given
// width and height=3 (y=0 -> red, y=1 -> green, y=2 -> blue):
Testimg *testimg_gen_three_color_space(const int width);

// create a full rgb color space of given width and fixed height=width*width:
Testimg *testimg_gen_rgb_space(const int width);


/*
 * Bad and nonsense value image generation
 */

// create greyscale pixels with max dynamic range values from FLT_MIN to FLT_MAX
// with height=1 (note: values are in the range ]0.0; +inf[):
Testimg *testimg_gen_grey_max_dr();

// create greyscale pixels with max dynamic range values from -FLT_MAX to
// -FLT_MIN and -0.0 with height=1 (note: values are in the range ]-inf; 0.0]):
Testimg *testimg_gen_grey_max_dr_neg();

// create 3 "grey'ish" gradients where in each one a color dominates and clips:
// height: 3, y=0 => red clips, y=1 => green clips, y=2 => blue clips
Testimg *testimg_gen_grey_with_rgb_clipping(const int width);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
