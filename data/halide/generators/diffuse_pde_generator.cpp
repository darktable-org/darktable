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
 * Halide generator for the heat PDE diffusion kernel used by
 * darktable's diffuse/sharpen module.
 *
 * This implements the anisotropic PDE solver from:
 *   https://www.researchgate.net/publication/220663968
 * modified for multi-scale wavelet framework.
 *
 * Per pixel, the algorithm:
 *   1. Fetches 3x3 stencil from HF and LF buffers (strided by mult)
 *   2. Computes gradients on LF and HF (centered finite differences)
 *   3. For 4 derivative orders, computes anisotropic diffusion kernels:
 *      - c2 = exp(-magnitude * anisotropy)
 *      - rotation matrix based on isotropy mode (isotrope/isophote/gradient)
 *      - builds 3x3 convolution kernel from rotation matrix
 *   4. Convolves kernels with LF (orders 0,1) and HF (orders 2,3)
 *   5. Regularizes variance, accumulates, produces output
 *
 * Uses flat 2D buffer layout (xi = width*4, y) for both CPU and GPU.
 * Channel index c = xi % 4, pixel x = xi / 4.
 *
 * The isotropy_type for each of the 4 orders is passed as int parameters:
 *   0 = ISOTROPE, 1 = ISOPHOTE, 2 = GRADIENT
 */

#include "Halide.h"

using namespace Halide;

class DiffusePDEGenerator : public Generator<DiffusePDEGenerator> {
public:
    // Input buffers: flat 2D (xi, y) where xi = width * 4
    Input<Buffer<float, 2>> HF{"HF"};        // high-frequency (wavelet detail)
    Input<Buffer<float, 2>> LF{"LF"};        // low-frequency (wavelet blur)
    Input<Buffer<uint8_t, 2>> mask_buf{"mask_buf"};  // per-pixel mask (width, height)

    // Scalar parameters
    Input<int32_t> width_pixels{"width_pixels"};
    Input<int32_t> height{"height"};
    Input<int32_t> mult{"mult"};              // stencil stride = 1 << scale
    Input<int32_t> has_mask{"has_mask"};       // boolean: use mask?

    // Anisotropy factors (4 orders): already squared user params
    Input<float> anisotropy_0{"anisotropy_0"};
    Input<float> anisotropy_1{"anisotropy_1"};
    Input<float> anisotropy_2{"anisotropy_2"};
    Input<float> anisotropy_3{"anisotropy_3"};

    // Isotropy types (4 orders): 0=ISOTROPE, 1=ISOPHOTE, 2=GRADIENT
    Input<int32_t> isotropy_0{"isotropy_0"};
    Input<int32_t> isotropy_1{"isotropy_1"};
    Input<int32_t> isotropy_2{"isotropy_2"};
    Input<int32_t> isotropy_3{"isotropy_3"};

    // ABCD coefficients (diffusion weights per order)
    Input<float> A{"A"};
    Input<float> B{"B"};
    Input<float> C{"C"};
    Input<float> D{"D"};

    // Other parameters
    Input<float> strength{"strength"};
    Input<float> regularization_factor{"regularization_factor"};
    Input<float> variance_threshold{"variance_threshold"};

    Output<Buffer<float, 2>> output{"output"};

    // Intermediate Funcs for schedule access
    Func neighbour_HF{"neighbour_HF"}, neighbour_LF{"neighbour_LF"};

    void generate() {
        Var xi("xi"), y("y");

        // Channel and pixel coordinates
        Expr ch = xi % 4;
        Expr px = xi / 4;

        // For 2D buffer access we need (xi_coord, y_coord)
        // Helper: clamped flat xi for a given pixel offset
        auto clamped_xi = [&](Expr pixel_x) -> Expr {
            return clamp(pixel_x, 0, width_pixels - 1) * 4 + ch;
        };
        auto clamped_y = [&](Expr pixel_y) -> Expr {
            return clamp(pixel_y, 0, height - 1);
        };

        // Fetch 3x3 stencil neighbors
        // Stencil offsets: -mult, 0, +mult in pixel space

        // Actually, let's just inline the 9 neighbor accesses directly.
        // For each of the 9 positions (ii,jj) where offset = (ii-1)*mult, (jj-1)*mult:

        // Fetch all 9 neighbors for HF and LF
        // Layout: [0]=(-1,-1), [1]=(0,-1), [2]=(+1,-1),
        //         [3]=(-1, 0), [4]=(0, 0), [5]=(+1, 0),
        //         [6]=(-1,+1), [7]=(0,+1), [8]=(+1,+1)
        // But in darktable convention: first index is row (x=vertical), second is col (y=horizontal)
        // i_neighbours maps: 0 -> row-mult, 1 -> row, 2 -> row+mult
        // j_neighbours maps: 0 -> col-mult, 1 -> col, 2 -> col+mult
        // pixel[3*ii+jj] where ii is row index, jj is col index
        // So pixel[0] = (row-mult, col-mult), pixel[1] = (row-mult, col), etc.
        // NOTE: in darktable, "x is vertical, y is horizontal"
        // row = y in our coords, col = px in our coords
        // i_neighbours = row offsets, j_neighbours = col offsets

        // darktable: i = row (our y), j = col (our px)
        // i_neighbours[0] = row - mult, [1] = row, [2] = row + mult
        // j_neighbours[0] = col - mult, [1] = col, [2] = col + mult
        // neighbor[3*ii+jj] accesses i_neighbours[ii] + j_neighbours[jj]
        // So ii indexes row, jj indexes col

        // fetch_rc(row_offset, col_offset) in pixel-space offsets
        auto fetch_rc = [&](const Input<Buffer<float, 2>> &buf,
                           int row_off, int col_off) -> Expr {
            Expr nx = clamped_xi(px + col_off * mult);
            Expr ny = clamped_y(y + row_off * mult);
            return buf(nx, ny);
        };

        // n[k] for k=0..8, k = 3*ii+jj, ii=row_idx(0,1,2), jj=col_idx(0,1,2)
        // row_off = ii-1, col_off = jj-1
        Expr hf[9], lf[9];
        for(int ii = 0; ii < 3; ii++) {
            for(int jj = 0; jj < 3; jj++) {
                hf[3*ii+jj] = fetch_rc(HF, ii-1, jj-1);
                lf[3*ii+jj] = fetch_rc(LF, ii-1, jj-1);
            }
        }

        // Gradients (centered finite differences)
        // find_gradients: xy[0] = (pixels[7] - pixels[1]) / 2  (x = vertical = row direction)
        //                 xy[1] = (pixels[5] - pixels[3]) / 2  (y = horizontal = col direction)
        Expr grad_lf_x = (lf[7] - lf[1]) / 2.0f;
        Expr grad_lf_y = (lf[5] - lf[3]) / 2.0f;
        Expr grad_hf_x = (hf[7] - hf[1]) / 2.0f;
        Expr grad_hf_y = (hf[5] - hf[3]) / 2.0f;

        // Magnitude and normalized direction for LF gradient
        Expr mag_grad = sqrt(grad_lf_x * grad_lf_x + grad_lf_y * grad_lf_y);
        Expr cos_grad = select(mag_grad != 0.0f, grad_lf_x / mag_grad, 1.0f);
        Expr sin_grad = select(mag_grad != 0.0f, grad_lf_y / mag_grad, 0.0f);
        Expr cos2_grad = cos_grad * cos_grad;
        Expr sin2_grad = sin_grad * sin_grad;
        Expr cossin_grad = cos_grad * sin_grad;

        // Magnitude and normalized direction for HF gradient (called "laplacian" in dt)
        Expr mag_lapl = sqrt(grad_hf_x * grad_hf_x + grad_hf_y * grad_hf_y);
        Expr cos_lapl = select(mag_lapl != 0.0f, grad_hf_x / mag_lapl, 1.0f);
        Expr sin_lapl = select(mag_lapl != 0.0f, grad_hf_y / mag_lapl, 0.0f);
        Expr cos2_lapl = cos_lapl * cos_lapl;
        Expr sin2_lapl = sin_lapl * sin_lapl;
        Expr cossin_lapl = cos_lapl * sin_lapl;

        // c2 = exp(-magnitude * anisotropy) for each of 4 orders
        // darktable uses a fast approximate exp via integer bit tricks.
        // We use Halide's fast_exp which is similarly approximate.
        // Orders 0,2 use grad magnitude; orders 1,3 use lapl magnitude.
        Expr c2_0 = fast_exp(-mag_grad * anisotropy_0);
        Expr c2_1 = fast_exp(-mag_lapl * anisotropy_1);
        Expr c2_2 = fast_exp(-mag_grad * anisotropy_2);
        Expr c2_3 = fast_exp(-mag_lapl * anisotropy_3);

        // Build 3x3 convolution kernels for each order based on isotropy type.
        // kernel[k] for k=0..8 is the 3x3 stencil weight.
        //
        // For ISOTROPE: kernel = [0.25, 0.5, 0.25, 0.5, -3, 0.5, 0.25, 0.5, 0.25]
        //
        // For ISOPHOTE with c2, cos2, sin2, cossin:
        //   a[0][0] = cos2 + c2*sin2,  a[1][1] = c2*cos2 + sin2
        //   a[0][1] = a[1][0] = (c2 - 1) * cossin
        //   Then build_matrix:
        //     b11 = a01/2, b13 = -b11, b22 = -2*(a00+a11)
        //     kernel = [b11, a11, b13, a00, b22, a00, b13, a11, b11]
        //
        // For GRADIENT: same as isophote but swapped:
        //   a[0][0] = c2*cos2 + sin2,  a[1][1] = cos2 + c2*sin2
        //   a[0][1] = a[1][0] = (1 - c2) * cossin

        // Helper: build kernel from isotropy_type, c2, cos2, sin2, cossin
        // Returns 9 Exprs for the kernel weights
        auto build_kern = [](Expr iso_type, Expr c2_val,
                            Expr cos2, Expr sin2, Expr cossin,
                            Expr kern[9]) {
            // Isophote rotation matrix elements
            Expr iso_a00 = cos2 + c2_val * sin2;
            Expr iso_a11 = c2_val * cos2 + sin2;
            Expr iso_a01 = (c2_val - 1.0f) * cossin;

            // Gradient rotation matrix elements (swapped)
            Expr grd_a00 = c2_val * cos2 + sin2;
            Expr grd_a11 = cos2 + c2_val * sin2;
            Expr grd_a01 = (1.0f - c2_val) * cossin;

            // Select based on isotropy type
            Expr a00 = select(iso_type == 0, 0.0f,  // isotrope: doesn't use matrix
                             iso_type == 1, iso_a00,
                             grd_a00);
            Expr a11 = select(iso_type == 0, 0.0f,
                             iso_type == 1, iso_a11,
                             grd_a11);
            Expr a01 = select(iso_type == 0, 0.0f,
                             iso_type == 1, iso_a01,
                             grd_a01);

            // build_matrix
            Expr b11 = a01 / 2.0f;
            Expr b13 = -b11;
            Expr b22 = -2.0f * (a00 + a11);

            // Anisotropic kernel
            Expr aniso_kern[9] = {b11, a11, b13, a00, b22, a00, b13, a11, b11};

            // Isotrope kernel (constant)
            float iso_kern[9] = {0.25f, 0.5f, 0.25f, 0.5f, -3.0f, 0.5f, 0.25f, 0.5f, 0.25f};

            for(int k = 0; k < 9; k++) {
                kern[k] = select(iso_type == 0, iso_kern[k], aniso_kern[k]);
            }
        };

        // Build kernels for each order
        // Orders 0, 2 use grad angles; orders 1, 3 use lapl angles
        Expr kern0[9], kern1[9], kern2[9], kern3[9];
        build_kern(isotropy_0, c2_0, cos2_grad, sin2_grad, cossin_grad, kern0);
        build_kern(isotropy_1, c2_1, cos2_lapl, sin2_lapl, cossin_lapl, kern1);
        build_kern(isotropy_2, c2_2, cos2_grad, sin2_grad, cossin_grad, kern2);
        build_kern(isotropy_3, c2_3, cos2_lapl, sin2_lapl, cossin_lapl, kern3);

        // Convolve: derivatives[k] = sum_j(kern_k[j] * buffer[j])
        // Orders 0,1 convolve with LF; orders 2,3 convolve with HF
        Expr deriv0 = cast<float>(0), deriv1 = cast<float>(0);
        Expr deriv2 = cast<float>(0), deriv3 = cast<float>(0);
        Expr variance = cast<float>(0);

        for(int k = 0; k < 9; k++) {
            deriv0 += kern0[k] * lf[k];
            deriv1 += kern1[k] * lf[k];
            deriv2 += kern2[k] * hf[k];
            deriv3 += kern3[k] * hf[k];
            variance += hf[k] * hf[k];
        }

        // Regularize variance
        Expr var_reg = variance_threshold + variance * regularization_factor;

        // Accumulate
        Expr acc = deriv0 * A + deriv1 * B + deriv2 * C + deriv3 * D;

        // Final output: max(HF*strength + acc/variance + LF, 0)
        Expr center_hf = hf[4];  // center pixel HF
        Expr center_lf = lf[4];  // center pixel LF
        Expr result = center_hf * strength + acc / var_reg + center_lf;

        // Mask handling: if mask is 0, just output HF + LF (passthrough)
        // mask is per-pixel (width, height), not per-channel
        Expr mask_val = select(has_mask != 0,
                              cast<int32_t>(mask_buf(px, y)),
                              1);
        output(xi, y) = select(mask_val != 0,
                              max(result, 0.0f),
                              center_hf + center_lf);
    }

    void schedule() {
        Var xi = output.args()[0];
        Var y  = output.args()[1];

        if(get_target().has_gpu_feature()) {
            Var xio("xio"), yo("yo");
            output.gpu_tile(xi, y, xio, yo, 64, 16);
        } else {
            // CPU: vectorize across channels/pixels, parallelize rows
            Var xio("xio"), xii("xii");
            output.split(xi, xio, xii, 32).vectorize(xii).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DiffusePDEGenerator, diffuse_pde)
