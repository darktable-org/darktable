# Unit testing with cmocka

This folder contains unit tests
([https://en.wikipedia.org/wiki/Unit_testing](URL)) for different code units of
darktable. The tests are written with the help of cmocka
([https://cmocka.org](URL)).  This is a unit testing framework designed for C
with the support of function mocking. By coincident the maintainer of cmocka is
also a contributor to darktable.

This README explains how unit tests are designed and how a developer can add its
own tests.


## How to build and run tests

Compiling and running unit tests is very easy. Each unit test suite
`test_<suite-name>.c` is built as own executable. It can be run as a single
program (getting all debug output) or together with all other tests (getting
just a summary).

To build unit tests, cmocka must be installed (on Ubuntu 18.04: "apt install
libcmocka-dev"). Then, build darktable with the option `-DBUILD_TESTING=ON`.
This tells cmake to build all tests together with the darktable binary. Once the
binary is built, you can also just type `make test_<suite-name>` in the build
folder. This will compile just the specified test suite (very useful while
developing tests since it is much faster than re-building the whole source
tree).

To run all tests together, type `make test` in the build folder. This will kick
ctest to run all tests and provide a summary output. To run a single test suite
and get all debug output, you can run the binary directly from the build folder,
e.g: `./src/tests/unittests/test_sample`.

## Design rules for unit tests

Good code quality requires some general rules in order to harmonize the
developers mindset.

### Editor settings
* Spaces instead of tabs
* Indentation width: 2 spaces
* Max line length: 80 chars (allowing some lines to exceed)

### General rules
* Readability/maintainability over speed (these are tests not production code!)
* Use basic data types (mostly float and int)
* K.I.S.S - keep it simple, stupid
* Never use random values (not deterministic and not reproducible)
* Prefer loops over code repetition (can be better scaled for increased test
  coverage)
* Encapsulate and increase re-usability of common code by putting it into the
  `utils/` files

### Directory structure
The structure of this folder is in analogy to the original source directory
structure. For example, unit tests for image processing modules shall thus be
placed into `iop/test_<module_name>.c`.

### When you find a bug...
Test code and bug fixes are not necessarily linked together, they might - and
often should - be done in separate pull requests. But to make the tests go into
the repository, they must pass the automated tests which are applied to each
pull request. To make this happen, the tests need a work-around for the bug. You
should mark the work-around with the trace macro `TR_BUG` (see
[Tracing](#tracing)) - ideally together with a github issue number, so that it
can be fixed together with the actual bug fix.


## Extra asserts in addition to cmocka

Cmocka already comes with a lot of assertion methods and in most cases this will
be sufficient. But for the cases when not: extra assertion macros shall be
placed into `util/asserts.h`. It is important to mask them by `#ifndef ...
#define ... #endif` to prevent potential future name clashes with original
cmocka asserts - should the same macros once be integrated into cmocka itself.


## Tracing

Tracing can help while debugging. The tracing functionality is covered in
`util/tracing.h`.  Current implementation is done very basic and does not
support any means of filtering traces for different trace levels since it is not
yet clear what will be exactly needed in future.

The most important trace macro is: `TR_STEP()`, which is used to tell the user
which test step is now performed. The idea is to later add a script that
collects all calls to `TR_STEP()` in order to creates small test specification.
So, the string overgiven to `TR_STEP()` should be a short and meaningful
statement of the next test step taken.

Other trace macros are `TR_NOTE` to express an important information, `TR_BUG`
to indicate that a work-around for a bug has been added to the test (see [When
you find a bug...](#when-you-find-a-bug)) and `TR_DEBUG` to provide debug
information.


## Test images

The easiest way to test a function against several input Pixels - or a range of
Pixels - is to feed it with an image. A nice side-effect of test images is that
they can be changed in size to scale the test coverage. The utility
`util/testimage.c` has been designed in order to generate simple and small test
images.

### Generation and lifetime

A test image can be allocated with `testimg_alloc()`, resulting in an empty
image of a given size.  The better way is to use any of the existing generation
methods `testimg_gen_...()` since they already fill the image with life and can
be re-used. The idea is to add more methods as needed, so that the test vectors
can be shared among different tests.

After being used, a test image must be free'd by calling `testimg_free()`.

### Pixel access

A Pixel is defined as float array of 4 pixels (0=Red, 1=Green, 2=Blue, 3=Mask).
The index 3 is special since it is used by some modules to hold the mask. Its
use in unit tests is not yet defined but to be compatible with the darktable
internal representation of images the size of test image pixels has been chosen
equivalent.

A test image is defined by width (x) and height (y) and an array of pixels. To
access a certain pixel, the function `get_pixel(test_image, x, y)` should be
used.

The following macros should be used to iterate over the pixels of a test image:
`for_testimg_pixels_p_xy(ti)` and `for_testimg_pixels_p_yx(ti)`. Both consist of
an outer and an inner loop of x (iterating over width) and y (iterating over
height). The first letter in `xy` and `yx` denotes the outer loop. Both macros
also define a variable `float *p = get_pixel(ti, x, y)` which you can directly
use to access the pixel values.

Example of a test step:
```
TR_STEP("verify greyscale is computed correctly");
ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
testimg_print(ti);  // not needed but shown here as example
for_testimg_pixels_p_yx(ti)  // this defines variables int x, int y and float *p
{
  float ret = compute_something(p);
  TR_DEBUG("pixel={%e, %e, %e} => ret=%e", p[0], p[1], p[2], ret);
  assert_float_equal(ret, expected_ret, E);
}
testimg_free(ti);
```

### Linear vs log scaling

Test images are generated in linear-RGB (linear in respect to human perception).
Linearity technically means additivity: `f(x+y) = f(x) + f(y)` - and homogenity:
`f(a*x) = a * f(x)`. In practice this means that e.g. doubling a pixel value in
the dark areas results in the same perceived difference as doubling a pixel
value in the bright areas (1EV).

Example: the following block shows a gradient image from dark (left) to bright
(right) of width 5 and height 1 with a dynamic range of 4 EV stops:

```
R: +6.25e-02 +1.25e-01 +2.50e-01 +5.00e-01 +1.00e+00
G: +6.25e-02 +1.25e-01 +2.50e-01 +5.00e-01 +1.00e+00
B: +6.25e-02 +1.25e-01 +2.50e-01 +5.00e-01 +1.00e+00
```

Please note that the rightmost value (1.0) represents pure white (or generally
the brightest pixel value). Each further step to the left is a division by 2.
This image would result in a linear gradient from dark grey (-4EV, left) to
white (0EV, right) if displayed e.g. on a monitor. That's how human perception
works.

In contrast, the following example is technically linear but not when displaying
it. We would call this image to be in log-RGB:

```
R: +0.00e+00 +2.00e-01 +4.00e-01 +6.00e-01 +8.00e-01 +1.00e+00
G: +0.00e+00 +2.00e-01 +4.00e-01 +6.00e-01 +8.00e-01 +1.00e+00
B: +0.00e+00 +2.00e-01 +4.00e-01 +6.00e-01 +8.00e-01 +1.00e+00
```

There are conversion functions available in `utils/testimg.h` to move from one
representation to the other. But generated test images are usually in linear RGB
representation.


## Process methods

Testing of the `process()` methods goes a bit beyond simple unit testing in
strict terms. But it is very powerful since with the mechanisms presented here
we can feed the `process()` methods with simple, small test images - independent
of any other module.

Writing unit tests for these methods usually needs more insight into the interns
of the algorithms than just simple unit testing. It might also potentially
produce much more code given the many input options of some modules. Thus the
tests for the `process()` are put into separate files `test_<module>_process.c`.
