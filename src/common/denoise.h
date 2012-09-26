/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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
 * accumulate similar patches to output buffer, given the two input buffers.
 * the weights will be accumulated into the fourth channel in the output image.
 */
void dt_nlm_accum(
    const float *edges,         // edge channel of the input buffer
    const float *input2,        // reference image to use this iteration
    const float *edges2,        // edge channel of the reference image
    float       *output,        // accumulate output here
    const int    width,         // image size (all have to match)
    const int    height,
    const int    P,             // radius of the patch to match
    const int    K,             // radius of the search area
    const float  sharpness,     // sharpness of denoising
    float       *tmp);          // temporary memory,
                                // must be sizeof(float)*width*dt_num_threads()
                                // and at least 16-byte aligned.

/*
 * do an iteration of non-local means accumulation
 * with a downscaled prior.
 */
void dt_nlm_accum_scaled(
    const float *edges,        // input features
    const float *input2,       // prior payload
    const float *edges2,       // prior features
    float       *output,       // accum buffer
    const int    width,
    const int    height,
    const int    prior_width,  // = s*width, s<1
    const int    prior_height,
    const int    P,            // patch size
    const int    K,            // search window size
    const float  sharpness,
    float       *tmp);

/*
 * normalize the output buffer after accumulation.
 * divides out the weights in the fourth channel.
 * also does luma/chroma blending with the input
 * if luma and chroma < 1.0
 */
void dt_nlm_normalize(
    const float *const input,    // original input buffer to blend with
    float       *const output,   // denoised buffer as of dt_nlm_accum output
    const int          width,    // image size
    const int          height,
    const float        luma,     // luma blend weight: 0.0 no denoising
    const float        chroma);  // chroma blend weight

/* same as above, but add to input instead of blending. */
void dt_nlm_normalize_add(
    const float *const input,    // original input buffer to blend with
    float       *const output,   // denoised buffer as of dt_nlm_accum output
    const int          width,    // image size
    const int          height,
    const float        luma,     // luma weight: 0.0 no impact
    const float        chroma);  // chroma weight

