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
    Based on the C++ implementation by Philipp Krahenbuhl (BSD-3-Clause),
    bundled in pydensecrf by Lucas Beyer (MIT):
      https://github.com/lucasb-eyer/pydensecrf

    Original algorithm:
      Philipp Krahenbuhl, Vladlen Koltun
      "Efficient Inference in Fully Connected CRFs with Gaussian
      Edge Potentials" (NIPS 2011)
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Refine a binary segmentation mask using Dense CRF.
 *
 * Takes unary probabilities (e.g. from sigmoid of SAM logits) and
 * an RGB guide image, runs mean-field inference to sharpen mask
 * boundaries along image edges.
 *
 * @param probabilities  Input/output: foreground probabilities [0,1],
 *                       [width * height] floats. Modified in-place.
 * @param rgb            Guide image, uint8 RGB interleaved [width * height * 3].
 * @param width          Image width.
 * @param height         Image height.
 * @param sigma_spatial  Spatial std-dev for the smoothness kernel (pixels).
 * @param sigma_rgb      Color std-dev for the bilateral kernel (0-255 scale).
 * @param w_spatial      Weight of the spatial (smoothness) kernel.
 * @param w_bilateral    Weight of the bilateral (appearance) kernel.
 * @param n_iterations   Number of mean-field iterations (typically 5-10).
 */
void dt_dense_crf_binary(float *probabilities,
                         const unsigned char *rgb,
                         int width, int height,
                         float sigma_spatial,
                         float sigma_rgb,
                         float w_spatial,
                         float w_bilateral,
                         int n_iterations);

#ifdef __cplusplus
}
#endif
