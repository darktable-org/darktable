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

#define HL_OPP_SENSOR_PLANES 3
static void _process_opposed_xtrans(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float clipval = 0.987f * data->clip;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int pwidth  = ((width + 2 ) / 3) + (2 * HL_BORDER);
  const int pheight = ((height + 2) / 3) + (2 * HL_BORDER);
  const size_t p_size = plane_size(pwidth, pheight);

  const size_t p_off  = (HL_BORDER * pwidth) + HL_BORDER;

  dt_iop_image_copy(out, in, width * height);

  dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2], 0.0f};
  // doesn't work with incorrect coeffs or no defined filter
  if((icoeffs[0] < 0.1f) || (icoeffs[1] < 0.1f) || (icoeffs[2] < 0.1f)) return;

  dt_times_t time0 = { 0 }, time1 = { 0 };
  dt_get_times(&time0);

  float *fbuffer = dt_alloc_align_float(HL_OPP_SENSOR_PLANES * 2 * p_size);

  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2], 0.0f}; 
  const dt_aligned_pixel_t coeffs = { powf(clips[0], 1.0f / 3.0f), powf(clips[1], 1.0f / 3.0f), powf(clips[2], 1.0f / 3.0f), 0.0f};

  float *plane[HL_OPP_SENSOR_PLANES];
  float *refavg[HL_OPP_SENSOR_PLANES];

  for(int i = 0; i < HL_OPP_SENSOR_PLANES; i++)
  {
    plane[i]  = fbuffer + i * p_size;
    dt_iop_image_fill(plane[i], 0.0f, pwidth, pheight, 1);
    refavg[i] = plane[i] + HL_OPP_SENSOR_PLANES * p_size; 
  }

  // test xtrans alignment on a 3x3 superpixel
  // FIXME we need correct alignment here
  float counter[3] = { 0.0f , 0.0f, 0.0f };  
  for(size_t row = 0; row < 3; row++)
  {
    for(size_t col = 0, i = row*width; col < 3; col++, i++)
    {
      const int c = FCxtrans(row, col, roi_in, xtrans);
      counter[c] += 1.0f;
    }
  }
  if((counter[0] != 2.0f) || (counter[1] != 5.0f) || (counter[2] != 2.0f))
  {
    fprintf(stderr, "xtrans alignment problem %i %i %i (should be 2 5 2), aligned at %i/%i\n", (int)counter[0], (int)counter[1], (int)counter[2], roi_in->x, roi_in->y);
  }

  const float corrections[3] = { 1.0f / counter[0], 1.0f / counter[1], 1.0f / counter[2] };  
  int has_clipped = 0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction( | : has_clipped) \
  dt_omp_firstprivate(in, plane, clips, corrections, roi_in) \
  dt_omp_sharedconst(width, p_off, height, pwidth, xtrans) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int c = FCxtrans(row, col, roi_in, xtrans);
      const size_t o = (row/3)*pwidth + (col/3) + p_off;
      const float ival = fmaxf(0.0f, in[i]);
      if(ival > clips[c]) has_clipped |= 1;
      const float val = corrections[c] * powf(ival, 1.0f / 3.0f);
      plane[c][o] += val;

      if(col >= width-2)      plane[c][o+1] += val;
      if(row >= height-2)     plane[c][o+pwidth] += val;
    }
  }

  if(!has_clipped) goto finish;

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static)
#endif
  for(size_t i = 0; i < pwidth * pheight; i++)
  {
    refavg[0][i] = 0.5f * (plane[1][i] + plane[2][i]);
    refavg[1][i] = 0.5f * (plane[0][i] + plane[2][i]);
    refavg[2][i] = 0.5f * (plane[0][i] + plane[1][i]);
  }

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};
  // Find channel average chrominances
  for(int c = 0; c < HL_OPP_SENSOR_PLANES; c++)
  {
    const float cutoff_dark = powf((-data->chrominance + 0.75f) * clips[c], 1.0f / 3.0f);
    double sum = 0.0f;
    double cnt = 0.0f;  
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction(+ : sum, cnt) \
  dt_omp_firstprivate(plane, coeffs, chrominance, refavg) \
  dt_omp_sharedconst(cutoff_dark, pwidth, pheight, c) \
  schedule(static)
#endif
    for(size_t row = HL_BORDER; row < pheight - HL_BORDER; row++)
    {
      for(size_t col = HL_BORDER; col < pwidth - HL_BORDER; col++)
      {
        const size_t i = row * pwidth + col;
        const float val = plane[c][i];
        if((val > cutoff_dark) && (val < coeffs[c]))
        {
          sum += (double) val - refavg[c][i];
          cnt += 1.0;
        }
      }
    }
    chrominance[c] = (float) sum / fmaxf(1.0f, cnt);
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, in, plane, clips, coeffs, refavg, roi_in, chrominance) \
  dt_omp_sharedconst(width, height, pwidth, p_off, xtrans) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, o = row * width; col < width; col++, o++)
    {
      const int c = FCxtrans(row, col, roi_in, xtrans);
      const float ival = fmaxf(0.0f, in[o]);
      if(ival > clips[c])
      {
        // FIXME we do need some blending here to avoid border artefacts
        const size_t i = (row/3)*pwidth + (col/3) + p_off;
        const float oval = fmaxf(coeffs[c], refavg[c][i] + chrominance[c]);
        out[o] = powf(oval, 3.0f);
      }
    }
  }

  finish:
  dt_get_times(&time1);
  dt_print(DT_DEBUG_PERF, "[inpaint xtrans opposed report] %.1fMpix in %.3fs%s\n",
       (float) (width * height) / 1.0e6f, time1.clock - time0.clock, (!has_clipped) ? ", no clipped data." : ".");

  dt_free_align(fbuffer);
}
static void _process_opposed_bayer(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data)
{
  const uint32_t filters = piece->pipe->dsc.filters;
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
  if((icoeffs[0] < 0.1f) || (icoeffs[1] < 0.1f) || (icoeffs[2] < 0.1f)) return;

  dt_times_t time0 = { 0 }, time1 = { 0 };
  dt_get_times(&time0);

  float *fbuffer = dt_alloc_align_float(HL_OPP_SENSOR_PLANES * 2 * p_size);

  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2], 0.0f}; 
  const dt_aligned_pixel_t coeffs = { powf(clips[0], 1.0f / 3.0f), powf(clips[1], 1.0f / 3.0f), powf(clips[2], 1.0f / 3.0f), 0.0f};

  float *plane[HL_OPP_SENSOR_PLANES];
  float *refavg[HL_OPP_SENSOR_PLANES];

  for(int i = 0; i < HL_OPP_SENSOR_PLANES; i++)
  {
    plane[i]  = fbuffer + i * p_size;
    dt_iop_image_fill(plane[i], 0.0f, pwidth, pheight, 1);
    refavg[i] = plane[i] + HL_OPP_SENSOR_PLANES * p_size; 
  }

  const float corrections[3] = { 1.0f, 0.5f, 1.0f };  
  int has_clipped = 0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction( | : has_clipped) \
  dt_omp_firstprivate(in, plane, clips, corrections) \
  dt_omp_sharedconst(width, p_off, height, pwidth, filters) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int c = FC(row, col, filters);
      const size_t o = (row/2)*pwidth + (col/2) + p_off;
      const float ival = fmaxf(0.0f, in[i]);
      if(ival > clips[c]) has_clipped |= 1;
      const float val = corrections[c] * powf(ival, 1.0f / 3.0f);
      plane[c][o] += val;

      if(col >= width-2)      plane[c][o+1] += val;
      if(row >= height-2)     plane[c][o+pwidth] += val;
    }
  }

  if(!has_clipped) goto finish;

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static)
#endif
  for(size_t i = 0; i < pwidth * pheight; i++)
  {
    refavg[0][i] = 0.5f * (plane[1][i] + plane[2][i]);
    refavg[1][i] = 0.5f * (plane[0][i] + plane[2][i]);
    refavg[2][i] = 0.5f * (plane[0][i] + plane[1][i]);
  }

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};
  // Find channel average chrominances
  for(int c = 0; c < HL_OPP_SENSOR_PLANES; c++)
  {
    const float cutoff_dark = powf((-data->chrominance + 0.75f) * clips[c], 1.0f / 3.0f);
    double sum = 0.0f;
    double cnt = 0.0f;  
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction(+ : sum, cnt) \
  dt_omp_firstprivate(plane, coeffs, chrominance, refavg) \
  dt_omp_sharedconst(cutoff_dark, pwidth, pheight, c) \
  schedule(static)
#endif
    for(size_t row = HL_BORDER; row < pheight - HL_BORDER; row++)
    {
      for(size_t col = HL_BORDER; col < pwidth - HL_BORDER; col++)
      {
        const size_t i = row * pwidth + col;
        const float val = plane[c][i];
        if((val > cutoff_dark) && (val < coeffs[c]))
        {
          sum += (double) val - refavg[c][i];
          cnt += 1.0;
        }
      }
    }
    chrominance[c] = (float) sum / fmaxf(1.0f, cnt);
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, in, plane, clips, coeffs, refavg, chrominance) \
  dt_omp_sharedconst(width, height, pwidth, p_off, filters) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, o = row * width; col < width; col++, o++)
    {
      const int c = FC(row, col, filters);
      const float ival = fmaxf(0.0f, in[o]);
      if(ival > clips[c])
      {
        const size_t i = (row/2)*pwidth + (col/2) + p_off;
        const float oval = fmaxf(coeffs[c], refavg[c][i] + chrominance[c]);
        out[o] = powf(oval, 3.0f);
      }
    }
  }

  finish:
  dt_get_times(&time1);
  dt_print(DT_DEBUG_PERF, "[inpaint bayer opposed report] %.1fMpix in %.3fs%s\n",
       (float) (width * height) / 1.0e6f, time1.clock - time0.clock, (!has_clipped) ? ", no clipped data." : ".");

  dt_free_align(fbuffer);
}

