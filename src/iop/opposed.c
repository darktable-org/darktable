/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#define HL_OPP_SENSOR_PLANES 4
static void _process_opposed(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const uint32_t filters, dt_iop_highlights_data_t *data)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float clipval = 0.987f * data->clip;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int pwidth  = ((width + 1 ) / 2) + (2 * HL_BORDER);
  const int pheight = ((height + 1) / 2) + (2 * HL_BORDER);
  const size_t p_size = plane_size(pwidth, pheight);

  const size_t p_off  = (HL_BORDER * pwidth) + HL_BORDER;

  dt_iop_image_copy(out, in, width * height);

  dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2], 0.0f};
  // doesn't work with incorrect coeffs or no defined filter
  if((filters == 0) || (icoeffs[0] < 0.1f) || (icoeffs[1] < 0.1f) || (icoeffs[2] < 0.1f)) return;

  dt_times_t time0 = { 0 }, time1 = { 0 };
  dt_get_times(&time0);

  float *fbuffer = dt_alloc_align_float(HL_OPP_SENSOR_PLANES * 2 * p_size);

  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t coeffs = { powf(clips[0], 1.0f / 3.0f), powf(clips[1], 1.0f / 3.0f), powf(clips[2], 1.0f / 3.0f), powf(clips[3], 1.0f / 3.0f)};

  float *plane[HL_OPP_SENSOR_PLANES];
  float *refavg[HL_OPP_SENSOR_PLANES];

  for(int i = 0; i < HL_OPP_SENSOR_PLANES; i++)
  {
    plane[i]  = fbuffer + i * p_size;
    refavg[i] = plane[i] + HL_OPP_SENSOR_PLANES * p_size; 
  }

/*
  We fill the planes [0-3] by the data from the photosites.
  These will be modified by the reconstruction algorithm and eventually written to out.
  The size of input rectangle can be odd meaning the planes might be not exactly of equal size
  so we possibly fill latest row/col by previous.
*/

  int has_clipped = 0;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  reduction( | : has_clipped) \
  dt_omp_firstprivate(in, plane, clips) \
  dt_omp_sharedconst(width, p_off, height, pwidth, filters) \
  schedule(static) aligned(in, plane : 64)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int p = _pos2plane(row, col, filters);
      const size_t o = (row/2)*pwidth + (col/2) + p_off;
      const float ival = fmaxf(0.0f, in[i]);
      if(ival > clips[p]) has_clipped |= 1;
      const float val = powf(ival, 1.0f / 3.0f);
      plane[p][o] = val;

      if(col >= width-2)      plane[p][o+1] = val;
      if(row >= height-2)     plane[p][o+pwidth] = val;
    }
  }

  if(!has_clipped) goto finish;

  for(int i = 0; i < HL_OPP_SENSOR_PLANES; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HL_BORDER);


  // Calculate opponent channel weighted means
  const float weights[4][4] = {
    { 0.0f, 0.25f, 0.25f, 0.5f },
    { 0.5f,  0.0f,  0.0f, 0.5f },
    { 0.5f,  0.0f,  0.0f, 0.5f },
    { 0.5f, 0.25f, 0.25f, 0.0f },
  };
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg, weights) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 0; row < pheight; row++)
  {
    for(size_t col = 0; col < pwidth; col++)
    {
      const size_t i = row * pwidth + col;
      for(int p = 0; p < HL_REF_PLANES; p++)
      {
        refavg[p][i] =
          weights[p][0] * plane[0][i] +
          weights[p][1] * plane[1][i] +
          weights[p][2] * plane[2][i] +
          weights[p][3] * plane[3][i];
      }
    }
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, in, plane, clips, coeffs, refavg) \
  dt_omp_sharedconst(width, height, pwidth, p_off, filters) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, o = row * width; col < width; col++, o++)
    {
      const int p = _pos2plane(row, col, filters);
      const float ival = fmaxf(0.0f, in[o]);
      if(ival > clips[p])
      {
        const size_t i = (row/2)*pwidth + (col/2) + p_off;
        const float oval = fmaxf(coeffs[p], refavg[p][i]);
        out[o] = powf(oval, 3.0f);
      }
    }
  }

  finish:
  dt_get_times(&time1);
  dt_print(DT_DEBUG_PERF, "[inpaint opposed] %.1fMpix. Tim3: %.3fs.%s\n",
       (float) (width * height) / 1.0e6f, time1.clock - time0.clock, (!has_clipped) ? " no clipped data" : "");

  dt_free_align(fbuffer);
}

