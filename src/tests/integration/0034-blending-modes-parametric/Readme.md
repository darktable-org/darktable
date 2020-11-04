 # Parametric blending modes tests

This series of tests aims at showing regressions on parametric blending.

They use parametric masks along with normal, multiply, addition and darken blending modes, for modules blended in RGB (exposure) and in Lab (color balance).

They don't use drawn or rasterized masks.

The expected result has been produced with a RelWithDebInfo build using GCC 10.
