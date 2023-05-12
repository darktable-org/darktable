/*
   This file is part of darktable,
   Copyright (C) 2010-2023 darktable developers.

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


typedef enum diffuse_reconstruct_variant_t
{
  DIFFUSE_RECONSTRUCT_RGB = 0,
  DIFFUSE_RECONSTRUCT_CHROMA
} diffuse_reconstruct_variant_t;


enum wavelets_scale_t
{
  ANY_SCALE   = 1 << 0, // any wavelets scale   : reconstruct += HF
  FIRST_SCALE = 1 << 1, // first wavelets scale : reconstruct = 0
  LAST_SCALE  = 1 << 2, // last wavelets scale  : reconstruct += residual
};


static unsigned int scale_type(const int s, const int scales)
{
  unsigned int scale = ANY_SCALE;
  if(s == 0) scale |= FIRST_SCALE;
  if(s == scales - 1) scale |= LAST_SCALE;
  return scale;
}

static void _interpolate_and_mask(const float *const restrict input,
                                  float *const restrict interpolated,
                                  float *const restrict clipping_mask,
                                  const dt_aligned_pixel_t clips,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width,
                                  const size_t height)
{
  // Bilinear interpolation
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, clips, filters, wb)  \
    dt_omp_sharedconst(input, interpolated, clipping_mask) \
    schedule(static)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t i_center = i * width;
      const float center = input[i_center + j];

      float R = 0.f;
      float G = 0.f;
      float B = 0.f;

      int R_clipped = 0;
      int G_clipped = 0;
      int B_clipped = 0;

      if(i == 0 || j == 0 || i == height - 1 || j == width - 1)
      {
        // We are on the image edges. We don't need to demosaic,
        // just set R = G = B = center and record clipping.
        // This will introduce a marginal error close to edges, mostly irrelevant
        // because we are dealing with local averages anyway, later on.
        // Also we remosaic the image at the end, so only the relevant channel gets picked.
        // Finally, it's unlikely that the borders of the image get clipped due to vignetting.
        R = G = B = center;
        R_clipped = G_clipped = B_clipped = (center > clips[c]);
      }
      else
      {
        const size_t i_prev = (i - 1) * width;
        const size_t i_next = (i + 1) * width;
        const size_t j_prev = (j - 1);
        const size_t j_next = (j + 1);

        const float north = input[i_prev + j];
        const float south = input[i_next + j];
        const float west = input[i_center + j_prev];
        const float east = input[i_center + j_next];

        const float north_east = input[i_prev + j_next];
        const float north_west = input[i_prev + j_prev];
        const float south_east = input[i_next + j_next];
        const float south_west = input[i_next + j_prev];

        if(c == GREEN) // green pixel
        {
          G = center;
          G_clipped = (center > clips[GREEN]);
        }
        else // non-green pixel
        {
          // interpolate inside an X/Y cross
          G = (north + south + east + west) / 4.f;
          G_clipped = (north > clips[GREEN] || south > clips[GREEN] || east > clips[GREEN] || west > clips[GREEN]);
        }

        if(c == RED ) // red pixel
        {
          R = center;
          R_clipped = (center > clips[RED]);
        }
        else // non-red pixel
        {
          if(FC(i - 1, j, filters) == RED && FC(i + 1, j, filters) == RED)
          {
            // we are on a red column, so interpolate column-wise
            R = (north + south) / 2.f;
            R_clipped = (north > clips[RED] || south > clips[RED]);
          }
          else if(FC(i, j - 1, filters) == RED && FC(i, j + 1, filters) == RED)
          {
            // we are on a red row, so interpolate row-wise
            R = (west + east) / 2.f;
            R_clipped = (west > clips[RED] || east > clips[RED]);
          }
          else
          {
            // we are on a blue row, so interpolate inside a square
            R = (north_west + north_east + south_east + south_west) / 4.f;
            R_clipped = (north_west > clips[RED] || north_east > clips[RED] || south_west > clips[RED]
                          || south_east > clips[RED]);
          }
        }

        if(c == BLUE ) // blue pixel
        {
          B = center;
          B_clipped = (center > clips[BLUE]);
        }
        else // non-blue pixel
        {
          if(FC(i - 1, j, filters) == BLUE && FC(i + 1, j, filters) == BLUE)
          {
            // we are on a blue column, so interpolate column-wise
            B = (north + south) / 2.f;
            B_clipped = (north > clips[BLUE] || south > clips[BLUE]);
          }
          else if(FC(i, j - 1, filters) == BLUE && FC(i, j + 1, filters) == BLUE)
          {
            // we are on a red row, so interpolate row-wise
            B = (west + east) / 2.f;
            B_clipped = (west > clips[BLUE] || east > clips[BLUE]);
          }
          else
          {
            // we are on a red row, so interpolate inside a square
            B = (north_west + north_east + south_east + south_west) / 4.f;

            B_clipped = (north_west > clips[BLUE] || north_east > clips[BLUE] || south_west > clips[BLUE]
                        || south_east > clips[BLUE]);
          }
        }
      }

      dt_aligned_pixel_t RGB = { R, G, B, sqrtf(sqf(R) + sqf(G) + sqf(B)) };
      dt_aligned_pixel_t clipped = { R_clipped, G_clipped, B_clipped, (R_clipped || G_clipped || B_clipped) };

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, wb))
      {
        const size_t idx = (i * width + j) * 4 + k;
        interpolated[idx] = fmaxf(RGB[k] / wb[k], 0.f);
        clipping_mask[idx] = clipped[k];
      }
    }
}

static void _remosaic_and_replace(const float *const restrict input,
                                  const float *const restrict interpolated,
                                  const float *const restrict clipping_mask,
                                  float *const restrict output,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width,
                                  const size_t height)
{
  // Take RGB ratios and norm, reconstruct RGB and remosaic the image
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, filters, wb)  \
    dt_omp_sharedconst(output, interpolated, input, clipping_mask) \
    schedule(static)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const float opacity = clipping_mask[index + ALPHA];
      output[idx] = opacity * fmaxf(interpolated[index + c] * wb[c], 0.f)
                    + (1.f - opacity) * input[idx];
    }
}

static inline void guide_laplacians(const float *const restrict high_freq,
                                    const float *const restrict low_freq,
                                    const float *const restrict clipping_mask,
                                    float *const restrict output,
                                    const size_t width,
                                    const size_t height,
                                    const int mult,
                                    const float noise_level,
                                    const int salt,
                                    const unsigned int scale,
                                    const float radius_sq)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, clipping_mask, HF, LF, height, width, mult, noise_level, salt, scale, radius_sq) \
    schedule(static)
#endif
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const int i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult), (int)0) * width,            // x - mult
          i * width,                                       // x
          MIN((int)(i + mult), (int)height - 1) * width }; // x + mult
    for(int j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const float alpha = clipping_mask[index + ALPHA];
      const float alpha_comp = 1.f - clipping_mask[index + ALPHA];

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      if(alpha > 0.f) // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult), (int)0),           // y - mult
              j,                                      // y
              MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the linear fit of the laplacian of chromaticity against the laplacian of the norm
        // that is the chromaticity filter guided by the norm

        // Get the local average per channel
        dt_aligned_pixel_t means_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(neighbour_pixel_HF, means_HF : 64))
          {
            means_HF[c] += neighbour_pixel_HF[k][c] / 9.f;
          }

        // Get the local variance per channel
        dt_aligned_pixel_t variance_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(variance_HF, neighbour_pixel_HF, means_HF))
          {
            variance_HF[c] += sqf(neighbour_pixel_HF[k][c] - means_HF[c]) / 9.f;
          }

        // Find the channel most likely to contain details = max( variance(HF) )
        size_t guiding_channel_HF = ALPHA;
        float guiding_value_HF = 0.f;
        for(size_t c = 0; c < 3; ++c)
        {
          if(variance_HF[c] > guiding_value_HF)
          {
            guiding_value_HF = variance_HF[c];
            guiding_channel_HF = c;
          }
        }

        // Compute the linear regression channel = f(guide)
        dt_aligned_pixel_t covariance_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(variance_HF, covariance_HF, neighbour_pixel_HF, means_HF))
          {
            covariance_HF[c] += (neighbour_pixel_HF[k][c] - means_HF[c])
                                * (neighbour_pixel_HF[k][guiding_channel_HF] - means_HF[guiding_channel_HF]) / 9.f;
          }

        const float scale_multiplier = 1.f / radius_sq;
        const dt_aligned_pixel_t alpha_ch = { clipping_mask[index + RED], clipping_mask[index + GREEN], clipping_mask[index + BLUE], clipping_mask[index + ALPHA] };

        dt_aligned_pixel_t a_HF, b_HF;
        for_each_channel(c, aligned(out, neighbour_pixel_HF, a_HF, b_HF, covariance_HF, variance_HF, means_HF, high_frequency, alpha_ch))
        {
          // Get a and b s.t. y = a * x + b, y = test data, x = guide
          a_HF[c] = fmaxf(covariance_HF[c] / (variance_HF[guiding_channel_HF]), 0.f);
          b_HF[c] = means_HF[c] - a_HF[c] * means_HF[guiding_channel_HF];

          high_frequency[c] = alpha_ch[c] * scale_multiplier * (a_HF[c] * high_frequency[guiding_channel_HF] + b_HF[c])
                            + (1.f - alpha_ch[c] * scale_multiplier) * high_frequency[c];
        }
      }

      if(scale & FIRST_SCALE)
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if(scale & LAST_SCALE)
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);
      }

      // Last step of RGB reconstruct : add noise
      if((scale & LAST_SCALE) && salt && alpha > 0.f)
      {
        // Init random number generator
        uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        dt_aligned_pixel_t noise = { 0.f };
        dt_aligned_pixel_t sigma = { 0.20f };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };

        for_each_channel(c,aligned(out, sigma)) sigma[c] = out[index + c] * noise_level;

        // create statistical noise
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, out + index, sigma, flip, state, noise);

        // Save the noisy interpolated image
        for_each_channel(c,aligned(out, noise: 64))
        {
          // Ensure the noise only brightens the image, since it's clipped
          noise[c] = out[index + c] + fabsf(noise[c] - out[index + c]);
          out[index + c] = fmaxf(alpha * noise[c] + alpha_comp * out[index + c], 0.f);
        }
      }

      if(scale & LAST_SCALE)
      {
        // Break the RGB channels into ratios/norm for the next step of reconstruction
        const float norm = fmaxf(sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE])), 1e-6f);
        for_each_channel(c, aligned(out : 64)) out[index + c] /= norm;
        out[index + ALPHA] = norm;
      }
    }
  }
}


static inline void heat_PDE_diffusion(const float *const restrict high_freq,
                                      const float *const restrict low_freq,
                                      const float *const restrict clipping_mask,
                                      float *const restrict output,
                                      const size_t width,
                                      const size_t height,
                                      const int mult,
                                      const uint8_t scale,
                                      const float first_order_factor)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, clipping_mask, HF, LF, height, width, mult, scale, first_order_factor) \
    schedule(static)
#endif
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult), (int)0) * width,            // x - mult
          i * width,                                       // x
          MIN((int)(i + mult), (int)height - 1) * width }; // x + mult
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const dt_aligned_pixel_t alpha = { clipping_mask[index + RED],
                                         clipping_mask[index + GREEN],
                                         clipping_mask[index + BLUE],
                                         clipping_mask[index + ALPHA] };

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      // The for_each_channel macro uses 4 floats SIMD instructions or 3 float regular ops,
      // depending on system. Since we don't want to diffuse the norm, make sure to store and restore it later.
      // This is not much of an issue when processing image at full-res, but more harmful since
      // we reconstruct highlights on a downscaled variant
      const float norm_backup = high_frequency[3];

      if(alpha[ALPHA] > 0.f)  // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult), (int)0),           // y - mult
              j,                                      // y
              MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF: 16))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the laplacian in the direction parallel to the steepest gradient on the norm
        float DT_ALIGNED_ARRAY anisotropic_kernel_isophote[9] = { 0.25f, 0.5f, 0.25f, 0.5f, -3.f, 0.5f, 0.25f, 0.5f, 0.25f };

        // Convolve the filter to get the laplacian
        dt_aligned_pixel_t laplacian_HF = { 0.f, 0.f, 0.f, 0.f };
        for(int k = 0; k < 9; k++)
        {
          for_each_channel(c, aligned(laplacian_HF, neighbour_pixel_HF:16) aligned(anisotropic_kernel_isophote: 64))
            laplacian_HF[c] += neighbour_pixel_HF[k][c] * anisotropic_kernel_isophote[k];
        }

        // Diffuse
        const dt_aligned_pixel_t multipliers_HF = { 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 0.f };
        for_each_channel(c, aligned(high_frequency, multipliers_HF, laplacian_HF, alpha))
          high_frequency[c] += alpha[c] * multipliers_HF[c] * (laplacian_HF[c] - first_order_factor * high_frequency[c]);

        // Restore. See above.
        high_frequency[3] = norm_backup;
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);

        // renormalize ratios
        if(alpha[ALPHA] > 0.f)
        {
          const float norm = sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE]));
          for_each_channel(c, aligned(out, LF, high_frequency : 64))
            out[index + c] /= (c != ALPHA && norm > 1e-4f) ? norm : 1.f;
        }

        // Last scale : reconstruct RGB from ratios and norm - norm stays in the 4th channel
        // we need it to evaluate the gradient
        for_four_channels(c, aligned(out))
          out[index + c] = (c == ALPHA) ? out[index + ALPHA] : out[index + c] * out[index + ALPHA];
      }
    }
  }
}

static inline gint wavelets_process(const float *const restrict in,
                                    float *const restrict reconstructed,
                                    const float *const restrict clipping_mask,
                                    const size_t width,
                                    const size_t height,
                                    const int scales,
                                    float *const restrict HF,
                                    float *const restrict LF_odd,
                                    float *const restrict LF_even,
                                    const diffuse_reconstruct_variant_t variant,
                                    const float noise_level,
                                    const int salt,
                                    const float first_order_factor)
{
  gint success = TRUE;

  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,

  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  for(int s = 0; s < scales; ++s)
  {
    //dt_print(DT_DEBUG_ALWAYS, "CPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline(buffer_in, HF, buffer_out, width, height, mult, tempbuf, padded_size);

    unsigned int current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));

    if(variant == DIFFUSE_RECONSTRUCT_RGB)
      guide_laplacians(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, noise_level, salt, current_scale_type, radius);
    else
      heat_PDE_diffusion(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, current_scale_type, first_order_factor);

    if(darktable.dump_pfm_module)
    {
      char name[64];
      sprintf(name, "scale-input-%i", s);
      dt_dump_pfm(name, buffer_in, width, height,  4 * sizeof(float), "highlights");

      sprintf(name, "scale-blur-%i", s);
      dt_dump_pfm(name, buffer_out, width, height,  4 * sizeof(float), "highlights");
    }
  }
  dt_free_align(tempbuf);

  return success;
}


static void process_laplacian_bayer(struct dt_iop_module_t *self,
                                    dt_dev_pixelpipe_iop_t *piece,
                                    const void *const restrict ivoid,
                                    void *const restrict ovoid,
                                    const dt_iop_roi_t *const roi_in,
                                    const dt_iop_roi_t *const roi_out,
                                    const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const uint32_t filters = piece->pipe->dsc.filters;
  dt_aligned_pixel_t wb = { 1.f, 1.f, 1.f, 1.f };
  if(piece->pipe->dsc.temperature.coeffs[0] != 0.f)
  {
    wb[0] = piece->pipe->dsc.temperature.coeffs[0];
    wb[1] = piece->pipe->dsc.temperature.coeffs[1];
    wb[2] = piece->pipe->dsc.temperature.coeffs[2];
  }

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t ds_height = height / DS_FACTOR;
  const size_t ds_width = width / DS_FACTOR;

  // [R, G, B, norm] for each pixel
  float *restrict interpolated, *restrict clipping_mask;
  // temp buffers for blurs. We will need to cycle between them for memory efficiency
  float *restrict LF_odd, *restrict LF_even, *restrict temp;
  // wavelets scales buffers
  float *restrict HF, *restrict ds_interpolated, *restrict ds_clipping_mask;

  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                 4 | DT_IMGSZ_INPUT, &interpolated,
                                 4 | DT_IMGSZ_INPUT, &clipping_mask,
                                 0, NULL))
  {
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out, 0);
    return;
  }

  const dt_iop_roi_t roi_ds = { .x = 0, .y = 0, .height = ds_height, .width = ds_width };
  if(!dt_iop_alloc_image_buffers(self, &roi_ds, &roi_ds,
                                 4 | DT_IMGSZ_INPUT, &LF_odd,
                                 4 | DT_IMGSZ_INPUT, &LF_even,
                                 4 | DT_IMGSZ_INPUT, &temp,
                                 4 | DT_IMGSZ_INPUT, &HF,
                                 4 | DT_IMGSZ_INPUT, &ds_interpolated,
                                 4 | DT_IMGSZ_INPUT, &ds_clipping_mask,
                                 0, NULL))
  {
    dt_free_align(interpolated);
    dt_free_align(clipping_mask);
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out, 0);
    return;
  }

  const float scale = fmaxf(DS_FACTOR * piece->iscale / (roi_in->scale), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;

  _interpolate_and_mask(input, interpolated, clipping_mask, clips, wb, filters, width, height);
  dt_box_mean(clipping_mask, height, width, 4, 2, 1);

  // Downsample
  interpolate_bilinear(clipping_mask, width, height, ds_clipping_mask, ds_width, ds_height, 4);
  interpolate_bilinear(interpolated, width, height, ds_interpolated, ds_width, ds_height, 4);

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    wavelets_process(ds_interpolated, temp, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                     LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    wavelets_process(temp, ds_interpolated, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                     LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
  }

  // Upsample
  interpolate_bilinear(ds_interpolated, ds_width, ds_height, interpolated, width, height, 4);
  _remosaic_and_replace(input, interpolated, clipping_mask, output, wb, filters, width, height);

  if(darktable.dump_pfm_module)
  {
    dt_dump_pfm("interpolated", interpolated, width, height,  4 * sizeof(float), "highlights");
    dt_dump_pfm("clipping_mask", clipping_mask, width, height,  4 * sizeof(float), "highlights");
  }

  dt_free_align(interpolated);
  dt_free_align(clipping_mask);
  dt_free_align(temp);
  dt_free_align(LF_even);
  dt_free_align(LF_odd);
  dt_free_align(HF);
  dt_free_align(ds_interpolated);
  dt_free_align(ds_clipping_mask);
}

#ifdef HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid,
                                         cl_mem in,
                                         cl_mem reconstructed,
                                         cl_mem clipping_mask,
                                         const size_t sizes[3],
                                         const int width,
                                         const int height,
                                         dt_iop_highlights_global_data_t *const gd,
                                         const int scales,
                                         cl_mem HF,
                                         cl_mem LF_odd,
                                         cl_mem LF_even,
                                         const diffuse_reconstruct_variant_t variant,
                                         const float noise_level,
                                         const int salt,
                                         const float solid_color)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  for(int s = 0; s < scales; ++s)
  {
    //dt_print(DT_DEBUG_ALWAYS, "GPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    // Compute wavelets low-frequency scales
    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_bspline_horizontal, 0,
      CLARG(buffer_in), CLARG(HF), CLARG(width), CLARG(height), CLARG(mult));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    if(err != CL_SUCCESS) return err;

    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_bspline_vertical, 0,
      CLARG(HF), CLARG(buffer_out), CLARG(width), CLARG(height), CLARG(mult));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    if(err != CL_SUCCESS) return err;

    // Compute wavelets high-frequency scales and backup the maximum of texture over the RGB channels
    // Note : HF = detail - LF
    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_wavelets_detail, 0,
      CLARG(buffer_in), CLARG(buffer_out), CLARG(HF), CLARG(width), CLARG(height));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_wavelets_detail, sizes);
    if(err != CL_SUCCESS) return err;

    unsigned int current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));

    // Compute wavelets low-frequency scales
    if(variant == DIFFUSE_RECONSTRUCT_RGB)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_highlights_guide_laplacians, 0,
        CLARG(HF), CLARG(buffer_out), CLARG(clipping_mask),
        CLARG(reconstructed), // read-only
        CLARG(reconstructed), // write-only
        CLARG(width), CLARG(height), CLARG(mult), CLARG(noise_level), CLARG(salt), CLARG(current_scale_type), CLARG(radius));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_guide_laplacians, sizes);
      if(err != CL_SUCCESS) return err;
    }
    else // DIFFUSE_RECONSTRUCT_CHROMA
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_highlights_diffuse_color, 0,
        CLARG(HF), CLARG(buffer_out), CLARG(clipping_mask),
        CLARG(reconstructed), // read-only
        CLARG(reconstructed), // write-only
        CLARG(width), CLARG(height), CLARG(mult), CLARG(current_scale_type), CLARG(solid_color));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_diffuse_color, sizes);
      if(err != CL_SUCCESS) return err;
    }
  }

  return err;
}

static cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self,
                                         dt_dev_pixelpipe_iop_t *piece,
                                         cl_mem dev_in,
                                         cl_mem dev_out,
                                         const dt_iop_roi_t *const roi_in,
                                         const dt_iop_roi_t *const roi_out,
                                         const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  const uint32_t filters = piece->pipe->dsc.filters;

  dt_aligned_pixel_t wb = { 1.f, 1.f, 1.f, 1.f };
  if(piece->pipe->dsc.temperature.coeffs[0] != 0.f)
  {
    wb[0] = piece->pipe->dsc.temperature.coeffs[0];
    wb[1] = piece->pipe->dsc.temperature.coeffs[1];
    wb[2] = piece->pipe->dsc.temperature.coeffs[2];
  }

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);  // [R, G, B, norm] for each pixel
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // [R, G, B, norm] for each pixel

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // need full size here for blurring

  const float scale = fmaxf(DS_FACTOR * piece->iscale / (roi_in->scale), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);

  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float*)clips);
  cl_mem wb_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float*)wb);

  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_bilinear_and_mask, 0,
    CLARG(dev_in), CLARG(interpolated), CLARG(temp),
    CLARG(clips_cl), CLARG(wb_cl), CLARG(filters), CLARG(roi_out->width), CLARG(roi_out->height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask, sizes);
  dt_opencl_release_mem_object(clips_cl);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_box_blur, 0, CLARG(temp), CLARG(clipping_mask),
    CLARG(roi_out->width), CLARG(roi_out->height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  // Downsample
  const int RGBa = TRUE;
  dt_opencl_set_kernel_args(devid, gd->kernel_interpolate_bilinear, 0,
    CLARG(clipping_mask), CLARG(width), CLARG(height), CLARG(ds_clipping_mask), CLARG(ds_width), CLARG(ds_height), CLARG(RGBa));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_args(devid, gd->kernel_interpolate_bilinear, 0,
    CLARG(interpolated), CLARG(width), CLARG(height), CLARG(ds_interpolated), CLARG(ds_width), CLARG(ds_height), CLARG(RGBa));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    err = wavelets_process_cl(devid, ds_interpolated, temp, ds_clipping_mask, ds_sizes, ds_width, ds_height, gd, scales, HF,
                              LF_odd, LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, ds_clipping_mask, ds_sizes, ds_width, ds_height, gd, scales, HF,
                              LF_odd, LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  // Upsample
  dt_opencl_set_kernel_args(devid, gd->kernel_interpolate_bilinear, 0,
    CLARG(ds_interpolated), CLARG(ds_width), CLARG(ds_height), CLARG(interpolated), CLARG(width), CLARG(height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  // Remosaic
  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_remosaic_and_replace, 0,
    CLARG(dev_in), CLARG(interpolated), CLARG(clipping_mask), CLARG(dev_out),
    CLARG(wb_cl), CLARG(filters), CLARG(width), CLARG(height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace, sizes);
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  if(wb_cl) dt_opencl_release_mem_object(wb_cl);
  if(interpolated) dt_opencl_release_mem_object(interpolated);
  if(clipping_mask) dt_opencl_release_mem_object(clipping_mask);
  if(temp) dt_opencl_release_mem_object(temp);
  if(LF_even) dt_opencl_release_mem_object(LF_even);
  if(LF_odd) dt_opencl_release_mem_object(LF_odd);
  if(HF) dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  return err;

error:
  if(wb_cl) dt_opencl_release_mem_object(wb_cl);
  if(interpolated) dt_opencl_release_mem_object(interpolated);
  if(clipping_mask) dt_opencl_release_mem_object(clipping_mask);
  if(temp) dt_opencl_release_mem_object(temp);
  if(LF_even) dt_opencl_release_mem_object(LF_even);
  if(LF_odd) dt_opencl_release_mem_object(LF_odd);
  if(HF) dt_opencl_release_mem_object(HF);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return err;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
