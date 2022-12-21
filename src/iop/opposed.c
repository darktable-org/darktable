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

static inline float _calc_linear_refavg(const float *in, const int color)
{
  const dt_aligned_pixel_t ins = { powf(fmaxf(0.0f, in[0]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[1]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[2]), 1.0f / HL_POWERF), 0.0f };
  const dt_aligned_pixel_t opp = { 0.5f*(ins[1]+ins[2]), 0.5f*(ins[0]+ins[2]), 0.5f*(ins[0]+ins[1]), 0.0f};

  return powf(opp[color], HL_POWERF);
}

static float _color_magic(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const float points = fmaxf(0.0f, (dsc->rawprepare.raw_black_level + dsc->rawprepare.raw_white_point)) / 32000.0f;
  const float coeffs = fmaxf(0.0f, (dsc->temperature.coeffs[0] + dsc->temperature.coeffs[1] + dsc->temperature.coeffs[2])) / 3.0f;
  return data->clip * (points + coeffs);
}

// A slightly modified version for sraws
static void _process_linear_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const input, float *const output,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data, const gboolean quality)
{
  const float clipval = 0.987f * data->clip;
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2] };   

  const size_t pwidth  = dt_round_size(roi_in->width / 3, 2) + 2 * HL_BORDER;
  const size_t pheight = dt_round_size(roi_in->height / 3, 2) + 2 * HL_BORDER;
  const size_t p_size =  dt_round_size(pwidth * pheight, 64);

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  if(feqf(_color_magic(piece), data->chroma_correction[0], 1e-6f))
  {
    for(int c=0; c<3; c++)
      chrominance[c] = data->chroma_correction[c+1];
  }
  else
  {
    int *mask_buffer = (quality) ? dt_calloc_align(64, 4 * p_size * sizeof(int)) : NULL;
    if(mask_buffer)
    {
      gboolean anyclipped = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( | : anyclipped) \
  dt_omp_firstprivate(clips, input, roi_in, mask_buffer) \
  dt_omp_sharedconst(p_size, pwidth) \
  schedule(static)
#endif
      for(size_t row = 1; row < roi_in->height -1; row++)
      {
        for(size_t col = 1; col < roi_in->width -1; col++)
        {
          const size_t idx = (row * roi_in->width + col) * 4;
          for_each_channel(c)
          {
            if(input[idx+c] >= clips[c])
            {
              mask_buffer[c * p_size + _raw_to_plane(pwidth, row, col)] |= 1;
              anyclipped |= TRUE;
            }
          }
        }
      }
      /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
      */
      if(anyclipped)
      {
        for(size_t i = 0; i < 3; i++)
        {
          int *mask = mask_buffer + i * p_size;
          int *tmp = mask_buffer + 3 * p_size;
          _intimage_borderfill(mask, pwidth, pheight, 0, HL_BORDER+1);
          _dilating(mask, tmp, pwidth, pheight, HL_BORDER, 3);
          memcpy(mask, tmp, p_size * sizeof(int));
        }
        dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
        dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, roi_in, clips, clipdark, mask_buffer) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(p_size, pwidth) \
  schedule(static)
#endif
        for(size_t row = 1; row < roi_in->height-1; row++)
        {
          for(size_t col = 1; col < roi_in->width - 1; col++)
          {
            const size_t idx = (row * roi_in->width + col) * 4;
            for_each_channel(c)
            {
              const float inval = fmaxf(0.0f, input[idx+c]); 
              if((inval > clipdark[c]) && (inval < clips[c]) && (mask_buffer[c * p_size + _raw_to_plane(pwidth, row, col)]))
              {
                cr_sum[c] += inval - _calc_linear_refavg(&input[idx], c);
                cr_cnt[c] += 1.0f;
              }
            }
          }
        }
        for_each_channel(c)
          chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);    
      }

      // also checking for an altered image to avoid xmp writing if not desired
      if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
         && (abs((int)(roi_out->width / roi_out->scale) - piece->buf_in.width) < 10)
         && (abs((int)(roi_out->height / roi_out->scale) - piece->buf_in.height) < 10)
         && dt_image_altered(piece->pipe->image.id))
      {
        dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
        for(int c = 0; c < 3; c++)
          data->chroma_correction[c+1] = p->chroma_correction[c+1] = chrominance[c];
        data->chroma_correction[0] = p->chroma_correction[0] = _color_magic(piece);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
      }
    }
    dt_free_align(mask_buffer);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(output, input, roi_in, roi_out, chrominance, clips) \
  schedule(static) collapse(2)
#endif
  for(ssize_t row = 0; row < roi_out->height; row++)
  {
    for(ssize_t col = 0; col < roi_out->width; col++)
    {
      const ssize_t odx = (row * roi_out->width + col) * 4;
      const ssize_t inrow = MIN(row, roi_in->height-1);
      const ssize_t incol = MIN(col, roi_in->width-1);
      const ssize_t idx = (inrow * roi_in->width + incol) * 4;
      for_each_channel(c)
      {
        const float ref = _calc_linear_refavg(&input[idx], c);
        const float inval = fmaxf(0.0f, input[idx+c]);
        output[odx+c] = (inval >= clips[c]) ? fmaxf(inval, ref + chrominance[c]) : inval;
      }
    }
  }
}

static float *_process_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const input, float *const output,
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
  const size_t p_size =  dt_round_size((size_t) pwidth * pheight, 64);

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  if(feqf(_color_magic(piece), data->chroma_correction[0], 1e-6f))
  {
    for(int c=0; c<3; c++)
      chrominance[c] = data->chroma_correction[c+1];
  }
  else
  {
    int *mask_buffer = (quality) ? dt_calloc_align(64, 4 * p_size * sizeof(int)) : NULL;
    if(mask_buffer)
    {
      gboolean anyclipped = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( | : anyclipped) \
  dt_omp_firstprivate(clips, input, roi_in, xtrans, mask_buffer) \
  dt_omp_sharedconst(filters, p_size, pwidth) \
  schedule(static) collapse(2)
#endif
      for(size_t row = 1; row < roi_in->height -1; row++)
      {
        for(size_t col = 1; col < roi_in->width -1; col++)
        {
          const size_t idx = row * roi_in->width + col;
          const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
          if(fmaxf(0.0f, input[idx]) >= clips[color])
          {
            mask_buffer[color * p_size + _raw_to_plane(pwidth, row, col)] |= 1;
            anyclipped |= TRUE;
          }
        }
      }
      /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
         If there are no clipped locations we keep the chrominance correction at 0 but make it valid 
      */
      if(anyclipped)
      {
        for(size_t i = 0; i < 3; i++)
        {
          int *mask = mask_buffer + i * p_size;
          int *tmpm = mask_buffer + 3 * p_size;
          _intimage_borderfill(mask, pwidth, pheight, 0, HL_BORDER+1);
          _dilating(mask, tmpm, pwidth, pheight, HL_BORDER, 3);
          memcpy(mask, tmpm, p_size * sizeof(int));
        }

        /* After having the surrounding mask for each color channel we can calculate the chrominance corrections. */ 
        dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
        dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, roi_in, xtrans, clips, clipdark, mask_buffer) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(filters, p_size, pwidth) \
  schedule(static) collapse(2)
#endif
        for(size_t row = 1; row < roi_in->height - 1; row++)
        {
          for(size_t col = 1; col < roi_in->width - 1; col++)
          {
            const size_t idx = row * roi_in->width + col;
            const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
            const float inval = fmaxf(0.0f, input[idx]); 
            /* we only use the unclipped photosites very close the true clipped data to calculate the chrominance offset */
            if((mask_buffer[color * p_size + _raw_to_plane(pwidth, row, col)]) && (inval > clipdark[color]) && (inval < clips[color]))
            {
              cr_sum[color] += inval - _calc_refavg(&input[idx], xtrans, filters, row, col, roi_in, TRUE);
              cr_cnt[color] += 1.0f;
            }
          }
        }
        for_each_channel(c)
          chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);
      }

      // also checking for an altered image to avoid xmp writing if not desired
      if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL) && dt_image_altered(piece->pipe->image.id))
      {
        dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
        for(int c = 0; c < 3; c++)
          data->chroma_correction[c+1] = p->chroma_correction[c+1] = chrominance[c];
        data->chroma_correction[0] = p->chroma_correction[0] = _color_magic(piece);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
      }
    }
    dt_free_align(mask_buffer);
  }
 
  float *tmpout = (keep) ? dt_alloc_align_float(roi_in->width * roi_in->height) : NULL;
  if(tmpout)
  {  
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, input, tmpout, roi_in, xtrans, chrominance) \
  dt_omp_sharedconst(filters) \
  schedule(static) collapse(2)
#endif
    for(size_t row = 0; row < roi_in->height; row++)
    {
      for(size_t col = 0; col < roi_in->width; col++)
      {
        const size_t idx = row * roi_in->width + col;
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float inval = fmaxf(0.0f, input[idx]);
        if((inval >= clips[color]) && (col > 0) && (col < roi_in->width - 1) && (row > 0) && (row < roi_in->height - 1))
        {
          const float ref = _calc_refavg(&input[idx], xtrans, filters, row, col, roi_in, TRUE);
          tmpout[idx] = fmaxf(inval, ref + chrominance[color]);
        }
        else
          tmpout[idx] = inval;
      }
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(output, input, tmpout, chrominance, clips, xtrans, roi_in, roi_out) \
  dt_omp_sharedconst(filters) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 0; row < roi_out->height; row++)
  {
    for(size_t col = 0; col < roi_out->width; col++) 
    {
      const size_t odx = row * roi_out->width + col;
      const size_t irow = row + roi_out->y;
      const size_t icol = col + roi_out->x;
      const size_t ix = irow * roi_in->width + icol;
      float oval = 0.0f;
      if((irow < roi_in->height) && (icol < roi_in->width))
      {
        if(tmpout)
          oval = tmpout[ix];
        else
        { 
          const int color = (filters == 9u) ? FCxtrans(irow, icol, roi_in, xtrans) : FC(irow, icol, filters);
          const gboolean inrefs = (irow > 0) && (icol > 0) && (irow < roi_in->height-1) && (icol < roi_in->width-1);
          oval = fmaxf(0.0f, input[ix]);
          if(inrefs && (oval >= clips[color]))
          {
            const float ref = _calc_refavg(&input[ix], xtrans, filters, irow, icol, roi_in, TRUE);
            oval = fmaxf(oval, ref + chrominance[color]);
          }
        }          
      }
      output[odx] = oval;
    }
  }
  return tmpout;
}

