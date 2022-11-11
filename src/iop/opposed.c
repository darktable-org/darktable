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

/* The refavg values are calculated in raw-RGB-cube3 space
   We calculate all color channels in the 3x3 photosite area, this can be understaood as a "superpixel",
   the "asking" location is in the centre.
   As this works for bayer and xtrans sensors we don't have a fixed ratio but calculate the average
   for every color channel first.
   refavg for one of red, green or blue is defined as means of both other color channels (opposing).
   
   The basic idea / observation for the _process_opposed algorithm is, the refavg is a good estimate
   for any clipped color channel in the vast majority of images, working mostly fine both for small specular
   highlighted spots and large areas.
   
   The correction via some sort of global chrominance further helps to correct color casts.
   The chrominace data are taken from the areas morphologically very close to clipped data.
   Failures of the algorithm (color casts) are in most cases related to
    a) very large differences between optimal white balance coefficients vs what we have as D65 in the darktable pipeline
    b) complicated lightings so the gradients are not well related
    c) a wrong whitepoint setting in the rawprepare module. 
    d) the maths might not be best

   Again the algorithm has been developed in collaboration by @garagecoder and @Iain from gmic team and @jenshannoschwalm from dt.
*/

static inline float _calc_linear_refavg(const float *in, const int row, const int col, const dt_iop_roi_t *const roi, const int color)
{
  dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
  for(int dy = -1; dy < 2; dy++)
  {
    for(int dx = -1; dx < 2; dx++)
    {
      for_each_channel(c)
        mean[c] += fmaxf(0.0f, in[roi->width * 4 * dy + 4 * dx + c]);
    }
  }
  for_each_channel(c)
    mean[c] = powf(mean[c] / 9.0f, 1.0f / HL_POWERF);

  const dt_aligned_pixel_t croot_refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};
  return powf(croot_refavg[color], HL_POWERF);
}

// A slightly modified version for sraws
static void _process_linear_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data, const gboolean quality)
{
  const float clipval = 0.987f * data->clip;
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2] };   

  const size_t pwidth  = dt_round_size(roi_in->width / 3, 2) + 2 * HL_BORDER;
  const size_t pheight = dt_round_size(roi_in->height / 3, 2) + 2 * HL_BORDER;
  const size_t p_size = (size_t) dt_round_size(pwidth * pheight, 16);

  const size_t shift_x = roi_out->x;
  const size_t shift_y = roi_out->y;

  const size_t o_row_max = MIN(roi_out->height, roi_in->height - shift_y);
  const size_t o_col_max = MIN(roi_out->width, roi_in->width - shift_x);
  const size_t o_width = roi_out->width;
  const size_t i_width = roi_in->width;

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};
  gboolean valid_chrominance = FALSE;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g && g->valid_chroma_correction)
  {
    valid_chrominance = TRUE;
    for_each_channel(c)
      chrominance[c] = g->chroma_correction[c];          
  }

  int *mask_buffer = dt_calloc_align(64, 4 * p_size * sizeof(int));
  float *tmpout = dt_alloc_align_float(4 * roi_in->width * roi_in->height);

  if(!tmpout || !mask_buffer)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ovoid, ivoid) \
  dt_omp_sharedconst(o_row_max, o_col_max, o_width, i_width, shift_x, shift_y) \
  schedule(static)
#endif
    for(size_t row = 0; row < o_row_max; row++)
    {
      float *out = (float *)ovoid + o_width * row * 4;
      float *in = (float *)ivoid + 4 * (i_width * (row + shift_y) + shift_x);
      for(size_t col = 0; col < o_col_max; col++)
      {
        for_each_channel(c)
          out[c] = fmaxf(0.0f, in[c]);
        out += 4;
        in += 4;
      }
    }
    goto finish;
  }

  size_t anyclipped = 0;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( + : anyclipped) \
  dt_omp_firstprivate(clips, ivoid, tmpout, roi_in, mask_buffer) \
  dt_omp_sharedconst(p_size, pwidth, pheight, i_width) \
  schedule(static)
#endif
  for(size_t row = 0; row < roi_in->height; row++)
  {
    float *tmp = tmpout + i_width * row * 4;
    float *in = (float *)ivoid + i_width * row * 4;
    for(size_t col = 0; col < i_width; col++)
    {
      for_each_channel(c)
        tmp[c] = fmaxf(0.0f, in[c]);

      if((col > 0) && (col < i_width - 1) && (row > 0) && (row < roi_in->height - 1))
      {
        for_each_channel(c)
        {
          if(in[c] >= clips[c])
          {
            tmp[c] = _calc_linear_refavg(&in[0], row, col, roi_in, c);
            mask_buffer[c * p_size + _raw_to_plane(pwidth, row, col)] |= 1;
            anyclipped += 1;
          }
        }
      }
      in += 4;
      tmp += 4;
    }
  }

  if(!valid_chrominance && (anyclipped > 5) && quality)
  {
    for(size_t i = 0; i < 3; i++)
    {
      int *mask = mask_buffer + i * p_size;
      int *tmp = mask_buffer + 3 * p_size;
      _intimage_borderfill(mask, pwidth, pheight, 0, HL_BORDER);
      _dilating(mask, tmp, pwidth, pheight, HL_BORDER, 3);
      memcpy(mask, tmp, p_size * sizeof(int));
    }

    dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
    dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, roi_in, clips, clipdark, mask_buffer) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(p_size, pwidth, i_width) \
  schedule(static)
#endif
    for(size_t row = 1; row < roi_in->height-1; row++)
    {
      float *in  = (float *)ivoid + i_width * row * 4 + 4;
      for(size_t col = 1; col < i_width - 1; col++)
      {
        for_each_channel(c)
        {
          const float inval = fmaxf(0.0f, in[c]); 
          if((mask_buffer[c * p_size + _raw_to_plane(pwidth, row, col)]) && (inval > clipdark[c]) && (inval < clips[c]))
          {
            cr_sum[c] += inval - _calc_linear_refavg(&in[0], row, col, roi_in, c);
            cr_cnt[c] += 1.0f;
          }
        }
        in += 4;
      }
    }
    for_each_channel(c)
      chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);    

    if(g && piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
    {
      for_each_channel(c)
        g->chroma_correction[c] = chrominance[c];
      g->valid_chroma_correction = TRUE;
    }
  }

/* We kept the refavg data in tmpout[] in the first loop, just overwrite output data with
   chrominance corrections now.
*/
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ovoid, ivoid, tmpout, chrominance, clips) \
  dt_omp_sharedconst(o_row_max, o_col_max, o_width, i_width, shift_x, shift_y) \
  schedule(static)
#endif
  for(size_t row = 0; row < o_row_max; row++)
  {
    float *out = (float *)ovoid + o_width * row * 4;
    float *tmp = tmpout + 4 * (i_width * (row + shift_y) + shift_x);
    float *in = (float *)ivoid + 4 * (i_width * (row + shift_y) + shift_x);
    for(size_t col = 0; col < o_col_max; col++)
    {
      for_each_channel(c)
      {
        const float inval = fmaxf(0.0f, in[c]);
        out[c] = (inval >= clips[c]) ? fmaxf(inval, tmp[c] + chrominance[c]) : inval;
      }
      out += 4;
      tmp += 4;
      in += 4;
    }
  }

  finish:
  dt_free_align(tmpout);
  dt_free_align(mask_buffer);
}

static float *_process_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data, const gboolean keep, const gboolean quality)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const float clipval = 0.987f * data->clip;
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};
  const dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2] };

  const size_t pwidth  = dt_round_size(roi_in->width / 3, 2) + 2 * HL_BORDER;
  const size_t pheight = dt_round_size(roi_in->height / 3, 2) + 2 * HL_BORDER;
  const size_t p_size = (size_t) dt_round_size((size_t) (pwidth + 4) * (pheight + 4), 16);

  int *mask_buffer = dt_calloc_align(64, 4 * p_size * sizeof(int));
  float *tmpout = dt_alloc_align_float(roi_in->width * roi_in->height);

  const size_t shift_x = roi_out->x;
  const size_t shift_y = roi_out->y;

  const size_t o_row_max = MIN(roi_out->height, roi_in->height - shift_y);
  const size_t o_col_max = MIN(roi_out->width, roi_in->width - shift_x);
  const size_t o_width = roi_out->width;
  const size_t i_width = roi_in->width;

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};
  gboolean valid_chrominance = FALSE;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g && g->valid_chroma_correction)
  {
    valid_chrominance = TRUE;
    for_each_channel(c)
      chrominance[c] = g->chroma_correction[c];          
  }

  if(!tmpout || !mask_buffer)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ovoid, ivoid) \
  dt_omp_sharedconst(o_row_max, o_col_max, o_width, i_width, shift_x, shift_y) \
  schedule(static)
#endif
    for(size_t row = 0; row < o_row_max; row++)
    {
      float *out = (float *)ovoid + o_width * row;
      float *in = (float *)ivoid + i_width * (row + shift_y) + shift_x;
      for(size_t col = 0; col < o_col_max; col++)
        out[col] = fmaxf(0.0f, in[col]);
    }
    dt_free_align(tmpout);
    dt_free_align(mask_buffer);
    return NULL;
  }

  size_t anyclipped = 0;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( + : anyclipped) \
  dt_omp_firstprivate(clips, ivoid, tmpout, roi_in, xtrans, mask_buffer) \
  dt_omp_sharedconst(filters, p_size, pwidth, pheight, i_width, o_width) \
  schedule(static)
#endif
  for(size_t row = 0; row < roi_in->height; row++)
  {
    float *tmp = tmpout + i_width * row;
    float *in = (float *)ivoid + i_width * row;
    for(size_t col = 0; col < i_width; col++)
    {
      const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
      tmp[0] = fmaxf(0.0f, in[0]);
      
      if((tmp[0] >= clips[color]) && (col > 0) && (col < i_width - 1) && (row > 0) && (row < roi_in->height - 1))
      {
        /* for the clipped photosites we later do the correction when the chrominance is available, we keep refavg in raw-RGB */
        tmp[0] = _calc_refavg(&in[0], xtrans, filters, row, col, roi_in, TRUE);
        mask_buffer[color * p_size + _raw_to_plane(pwidth, row, col)] |= 1;
        anyclipped += 1;
      }
      tmp++;
      in++;
    }
  }

  if(!valid_chrominance && (anyclipped > 5) && quality)
  {
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

  /* After having the surrounding mask for each color channel we can calculate the chrominance corrections. */ 
    dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
    dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, roi_in, xtrans, clips, clipdark, mask_buffer) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(filters, p_size, pwidth, i_width) \
  schedule(static)
#endif
    for(size_t row = 1; row < roi_in->height - 1; row++)
    {
      float *in  = (float *)ivoid + i_width * row + 1;
      for(size_t col = 1; col < i_width - 1; col++)
      {
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float inval = fmaxf(0.0f, in[0]); 
        /* we only use the unclipped photosites very close the true clipped data
           to calculate the chrominance offset */
        if((mask_buffer[color * p_size + _raw_to_plane(pwidth, row, col)]) && (inval > clipdark[color]) && (inval < clips[color]))
        {
          cr_sum[color] += inval - _calc_refavg(&in[0], xtrans, filters, row, col, roi_in, TRUE);
          cr_cnt[color] += 1.0f;
        }
        in++;
      }
    }
    for_each_channel(c)
      chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);

    // fprintf(stderr, "[opposed chroma corrections] %f, %f, %f\n", chrominance[0], chrominance[1], chrominance[2]);          

    if(g && piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
    {
      for_each_channel(c)
        g->chroma_correction[c] = chrominance[c];
      g->valid_chroma_correction = TRUE;
    }
  }

  if(keep && anyclipped)
  {
/* We kept the refavg data in tmpout[] in the first loop, just overwrite output data with
   chrominance corrections now. Also leave in tmpout for further postprocessing.
*/
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, ivoid, tmpout, roi_in, xtrans, chrominance) \
  dt_omp_sharedconst(filters, o_row_max, o_col_max, i_width) \
  schedule(static)
#endif
    for(size_t row = 0; row < roi_in->height; row++)
    {
      float *in = (float *)ivoid + i_width * row;
      float *tmp = tmpout + i_width * row;
      for(size_t col = 0; col < i_width; col++)
      {
        const float inval = fmaxf(0.0f, in[0]);
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        if(inval > clips[color])
          tmp[0] = fmaxf(inval, tmp[0] + chrominance[color]);
        in++;
        tmp++;
      }
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ovoid, tmpout) \
  dt_omp_sharedconst(o_row_max, o_col_max, i_width, o_width, shift_x, shift_y) \
  schedule(static)
#endif
    for(size_t row = 0; row < o_row_max; row++)
    {
      float *out = (float *)ovoid + o_width * row;
      float *tmp = tmpout + i_width * (row + shift_y) + shift_x;
      for(size_t col = 0; col < o_col_max; col++)
        out[col] = tmp[col];
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ovoid, ivoid, tmpout, chrominance, clips, xtrans, roi_in) \
  dt_omp_sharedconst(filters, o_row_max, o_col_max, o_width, i_width, shift_x, shift_y) \
  schedule(static)
#endif
    for(size_t row = 0; row < o_row_max; row++)
    {
      float *out = (float *)ovoid + o_width * row;
      float *tmp = tmpout + i_width * (row + shift_y) + shift_x;
      float *in = (float *)ivoid + i_width * (row + shift_y) + shift_x;
      for(size_t col = 0; col < o_col_max; col++)
      {
        const float inval = fmaxf(0.0f, in[col]);
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        out[col] = (inval >= clips[color]) ? fmaxf(inval, tmp[col] + chrominance[color]) : inval;
      }
    }
  }

  dt_free_align(mask_buffer);
  return tmpout;
}

