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

/* The refavg value are in raw-RGB-cube3 space
   We calculate all color channels in the 3x3 photosite area, the "asking" location is in the centre.
   As this should work for bayer and xtrans sensors we don't have a fixed ratio but calculate the average
   for every color channel first.
   refavg is defined as mean of opposing color channels.
   
   The basic idea / observation for the _process_opposed algorithm is, the refavg is a good estimate
   for any clipped color channel in the vast majority of images, working mostly fine both for small specular
   highlighted spots and large areas.
   
   The correction via some sort of global chrominance further helps to correct color casts.
   Remaining casts are in most cases related to
    a) very large differences between optimal white balance coefficients vs what we have as D65 in the darktable pipeline
    b) complicated lightings so the gradients are not well related
    c) a wrong whitepoint setting in the rawprepare module. 
   
   Again the algorithms has been developed in collaboration by @garagecoder and @Iain from gmic team an @jenshannoschwalm from dt.
*/

static inline float _calc_refavg(const float *in, const uint8_t(*const xtrans)[6], const uint32_t filters, const int row, const int col, const dt_iop_roi_t *const roi)
{
  const int color = (filters == 9u) ? FCxtrans(row, col, roi, xtrans) : FC(row, col, filters);
  dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f };
  for(int dy = -1; dy < 2; dy++)
  {
    for(int dx = -1; dx < 2; dx++)
    {
      const float val = in[(ssize_t)dy * roi->width + dx];
      const int c = (filters == 9u) ? FCxtrans(row + dy, col + dx, roi, xtrans) : FC(row + dy, col + dx, filters);
      mean[c] += val;
      cnt[c] += 1.0f;
    }
  }
  for(int c = 0; c < 3; c++)
    mean[c] = powf(mean[c] / cnt[c], 1.0f / 3.0f);
  const dt_aligned_pixel_t refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};
  return refavg[color];
}

static void _process_opposed(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const float clipval = 0.987f * data->clip;
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 

  const int pwidth  = dt_round_size(roi_in->width / 3, 4) + 2 * HL_BORDER;
  const int pheight = dt_round_size(roi_in->height / 3, 4) + 2 * HL_BORDER;
  const size_t p_size = (size_t) dt_round_size(pwidth * pheight, 16);
  const size_t p_off  = (HL_BORDER * pwidth) + HL_BORDER;

  int *mask_buffer = dt_calloc_align(64, 4 * p_size * sizeof(int));
  gboolean anyclipped = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( | : anyclipped) \
  dt_omp_firstprivate(clips, ivoid, ovoid, roi_in, roi_out, xtrans, mask_buffer) \
  dt_omp_sharedconst(filters, p_size, pwidth, pheight, p_off) \
  schedule(static)
#endif
  for(int row = 0; row < roi_out->height; row++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * row;
    float *in = (float *)ivoid + (size_t)roi_in->width * row;
    for(int col = 0; col < roi_out->width; col++)
    {
      const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
      out[0] = fmaxf(0.0f, in[0]);
      
      if((out[0] >= clips[color]) && (col > 0) && (col < roi_out->width - 1) && (row > 0) && (row < roi_out->height - 1))
      {
        const float refavg = _calc_refavg(&in[0], xtrans, filters, row, col, roi_in);
        /* for the clipped photosites we later do the correction when the chrominance is available, we keep refavg in raw-RGB */
        out[0] = powf(refavg, 3.0f);
        mask_buffer[color * p_size + p_off + (row/3) * pwidth + (col/3)] |= 1;
        anyclipped |= TRUE;
      }
      out++;
      in++;
    }
  }

  /* We want to use the photosites closely around clipped data to be taken into account.
     The mask buffers holds data for each color channel, we dilate the mask buffer slightly
     to get those locations.
     As the mask buffers are scaled down by 3 the dilate is very fast. 
  */      
  for(size_t i = 0; i < 3; i++)
  {
    int *mask = mask_buffer + i * p_size;
    int *tmp = mask_buffer + 3 * p_size;
    _intimage_borderfill(mask, pwidth, pheight, 0, HL_BORDER);
    _dilating(mask, tmp, pwidth, pheight, HL_BORDER, 3);
    memcpy(mask, tmp, p_size * sizeof(int));
  }

/* As the masks for each color channel are now prepared we can calculate the chrominance corrections.
   To avoid rounding errors doubles are used.
*/ 
  double cr_sum[3] = {0.0f, 0.0f, 0.0f};
  double cr_cnt[3] = {0.0f, 0.0f, 0.0f};
  if(anyclipped)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, roi_in, xtrans, clips, mask_buffer) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(filters, p_size, pwidth, p_off) \
  schedule(static)
#endif
    for(int row = 1; row < roi_in->height-1; row++)
    {
      float *in  = (float *)ivoid + (size_t)roi_in->width * row + 1;
      for(int col = 1; col < roi_in->width-1; col++)
      {
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float inval = fmaxf(0.0f, in[0]); 
        /* we only use the unclipped photosites very close the true clipped data
           to calculate the chrominance offset */
        if((mask_buffer[color * p_size + p_off + ((row/3) * pwidth) + (col/3)] != 0) && (inval < clips[color]))
        {
          const float refavg = powf(_calc_refavg(&in[0], xtrans, filters, row, col, roi_in), 3.0f);
          cr_sum[color] += (double) (inval - refavg);
          cr_cnt[color] += 1.0;
        }
        in++;
      }
    }
  }
  const dt_aligned_pixel_t chrominance = {cr_sum[0] / fmax(1.0, cr_cnt[0]), cr_sum[1] / fmax(1.0, cr_cnt[1]), cr_sum[2] / fmax(1.0, cr_cnt[2])};

/* We kept the refavg data in out[] in the first loop, just overwrite output data with
   chrominance corrections now.
*/
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, ivoid, ovoid, roi_in, roi_out, xtrans, chrominance) \
  dt_omp_sharedconst(filters) \
  schedule(static)
#endif
  for(int row = 1; row < roi_out->height-1; row++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * row + 1;
    float *in = (float *)ivoid + (size_t)roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width-1; col++)
    {
      const float ival = fmaxf(0.0f, in[0]);
      const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
      if(ival > clips[color])
        out[0] = fmaxf(ival, out[0] + chrominance[color]);
 
      out++;
      in++;
    }
  }
  dt_free_align(mask_buffer);
}


