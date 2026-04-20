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
 * Halide generator for B-spline wavelet decomposition used by
 * darktable's diffuse/sharpen module.
 *
 * Algorithm: separable 5-tap [1,4,6,4,1]/16 blur with strided access
 * (à-trous wavelet transform). Produces both the low-frequency (blurred)
 * and high-frequency (detail = input - blurred) components.
 *
 * darktable pixel layout: interleaved float RGBA, 4 channels per pixel.
 * We use the "flat" 2D approach (xi = x*4+c, y) that works on both
 * CPU and GPU, matching the exposure_flat pattern.
 *
 * The stride parameter "mult" controls the spacing between filter taps:
 *   mult = 1 << scale  (scale 0 -> mult=1, scale 1 -> mult=2, etc.)
 *
 * Boundary handling: clamp to edge (matching darktable's MIN/MAX clamping).
 *
 * Negative values in the blur output are clipped to 0 (matching darktable's
 * clip_negatives=TRUE in decompose_2D_Bspline).
 */

#include "Halide.h"

using namespace Halide;

// Flat 2D layout (xi, y), xi = width * 4.
// Separable filter: vertical pass then horizontal pass.
// mult is the tap spacing in pixels (= 1 << scale).
// width_pixels is the image width in pixels (not floats).
class BsplineDecomposeGenerator : public Generator<BsplineDecomposeGenerator> {
public:
    Input<Buffer<float, 2>> input{"input"};       // (xi, y) where xi = width*4
    Input<int32_t> width_pixels{"width_pixels"};  // image width in pixels
    Input<int32_t> height{"height"};              // image height in pixels
    Input<int32_t> mult{"mult"};                  // tap spacing = 1 << scale
    Output<Buffer<float, 2>> lf{"lf"};            // low-frequency (blurred)
    Output<Buffer<float, 2>> hf{"hf"};            // high-frequency (detail)

    // Intermediate stage accessible to schedule()
    Func vert{"vert"};

    void generate() {
        Var xi("xi"), y("y");

        // B-spline filter weights
        const float w0 = 1.0f / 16.0f;
        const float w1 = 4.0f / 16.0f;
        const float w2 = 6.0f / 16.0f;

        // Extract pixel x and channel from flat index
        Expr px = xi / 4;
        Expr ch = xi % 4;

        // Vertical pass: blur along y with spacing mult
        // Clamp row indices to [0, height-1]
        Expr r0 = clamp(y - 2 * mult, 0, height - 1);
        Expr r1 = clamp(y - mult, 0, height - 1);
        Expr r2 = y;
        Expr r3 = clamp(y + mult, 0, height - 1);
        Expr r4 = clamp(y + 2 * mult, 0, height - 1);

        vert(xi, y) = w0 * input(xi, r0)
                    + w1 * input(xi, r1)
                    + w2 * input(xi, r2)
                    + w1 * input(xi, r3)
                    + w0 * input(xi, r4);

        // Horizontal pass: blur along x with spacing mult on vertical result
        // Clamp at pixel boundaries, preserving channel offset
        Expr p0 = clamp(px - 2 * mult, 0, width_pixels - 1) * 4 + ch;
        Expr p1 = clamp(px - mult, 0, width_pixels - 1) * 4 + ch;
        Expr p2 = xi;
        Expr p3 = clamp(px + mult, 0, width_pixels - 1) * 4 + ch;
        Expr p4 = clamp(px + 2 * mult, 0, width_pixels - 1) * 4 + ch;

        Expr blur = w0 * vert(p0, y)
                  + w1 * vert(p1, y)
                  + w2 * vert(p2, y)
                  + w1 * vert(p3, y)
                  + w0 * vert(p4, y);

        // Clip negatives (matching darktable behavior)
        lf(xi, y) = max(blur, 0.0f);
        hf(xi, y) = input(xi, y) - lf(xi, y);
    }

    void schedule() {
        Var xi = lf.args()[0];
        Var y  = lf.args()[1];

        if(get_target().has_gpu_feature()) {
            // GPU schedule: compute vert inline (fused into the horizontal pass)
            // then tile the outputs
            Var xio("xio"), yo("yo");
            vert.compute_at(lf, yo);
            lf.gpu_tile(xi, y, xio, yo, 64, 16);
            hf.gpu_tile(xi, y, xio, yo, 64, 16);
        } else {
            // CPU schedule: compute vert per row strip for cache locality,
            // vectorize, parallelize over rows
            Var xio("xio"), xii("xii");

            // Compute vertical pass per output row (store full row)
            vert.compute_at(lf, y).vectorize(xi, 32);

            lf.split(xi, xio, xii, 32).vectorize(xii).parallel(y);
            // hf reads lf, so compute hf after lf at the same row
            hf.split(xi, xio, xii, 32).vectorize(xii).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(BsplineDecomposeGenerator, bspline_decompose)
