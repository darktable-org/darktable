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

/*
 * Halide generator for darktable's exposure module.
 *
 * Algorithm: out[c] = (in[c] - black) * scale  for c = R,G,B
 *            out[alpha] = in[alpha]  (passthrough)
 *
 * darktable pixel layout: interleaved float RGBA, 4 channels per pixel.
 * Memory layout: [R0,G0,B0,A0, R1,G1,B1,A1, ...]
 *                stride: c=1, x=4, y=4*width
 *
 * Two generators:
 *   "exposure"       — 3D (c,x,y) for CPU with SIMD vectorization
 *   "exposure_flat"  — 2D (xi,y) for Metal GPU, where xi = x*4+c
 *                      This avoids stride issues with Metal's buffer handling.
 */

#include "Halide.h"

using namespace Halide;

// CPU generator: 3D (c, x, y) with interleaved layout
class ExposureGenerator : public Generator<ExposureGenerator> {
public:
    Input<Buffer<float, 3>> input{"input"};
    Input<float> black{"black"};
    Input<float> scale{"scale"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Var c("c"), x("x"), y("y");

        Expr val = input(c, x, y);
        Expr adjusted = (val - black) * scale;
        output(c, x, y) = select(c < 3, adjusted, val);
    }

    void schedule() {
        Var c = output.args()[0];
        Var x = output.args()[1];
        Var y = output.args()[2];

        output.bound(c, 0, 4);
        input.dim(0).set_bounds(0, 4);

        // CPU schedule: parallel over rows, vectorize over x
        output.reorder(c, x, y);
        output.vectorize(c, 4);

        Var xo("xo"), xi("xi");
        output.split(x, xo, xi, 8);
        output.vectorize(xi);
        output.parallel(y);
    }
};

// GPU generator: 2D flat buffer (xi, y) where xi covers width*4 floats per row.
// Every 4th element (xi % 4 == 3) is alpha and passed through unchanged.
class ExposureFlatGenerator : public Generator<ExposureFlatGenerator> {
public:
    Input<Buffer<float, 2>> input{"input"};
    Input<float> black{"black"};
    Input<float> scale{"scale"};
    Output<Buffer<float, 2>> output{"output"};

    void generate() {
        Var xi("xi"), y("y");

        Expr val = input(xi, y);
        Expr adjusted = (val - black) * scale;
        // Alpha passthrough: every 4th element (index 3, 7, 11, ...) is alpha
        output(xi, y) = select((xi % 4) != 3, adjusted, val);
    }

    void schedule() {
        Var xi = output.args()[0];
        Var y  = output.args()[1];

        if(get_target().has_gpu_feature()) {
            Var xio("xio"), xii("xii"), yo("yo"), yi("yi");
            // Tile: 64 floats wide (16 pixels * 4 channels) x 16 rows
            output.gpu_tile(xi, y, xio, yo, 64, 16);
        } else {
            // Fallback CPU schedule (shouldn't normally be used)
            output.vectorize(xi, 16);
            output.parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(ExposureGenerator, exposure)
HALIDE_REGISTER_GENERATOR(ExposureFlatGenerator, exposure_flat)
