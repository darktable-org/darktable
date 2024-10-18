/*
    This file is part of darktable,
    Copyright (C) 2022-2024 darktable developers.

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

static dt_hash_t _opposed_parhash(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  dt_iop_highlights_data_t *d = piece->data;

  dt_hash_t hash = dt_hash(DT_INITHASH, &dsc->rawprepare, sizeof(dsc->rawprepare));
  hash = dt_hash(hash, &dsc->temperature, sizeof(dsc->temperature));
  hash = dt_hash(hash, &d->clip, sizeof(d->clip));
  return dt_hash(hash, &piece->module->dev->chroma.late_correction, sizeof(int));
}

static dt_hash_t _opposed_hash(dt_dev_pixelpipe_iop_t *piece)
{
  dt_hash_t hash = _opposed_parhash(piece);
  return dt_hash(hash, &piece->pipe->image.id, sizeof(piece->pipe->image.id));
}

static inline float _calc_linear_refavg(const float *in, const int color)
{
  const dt_aligned_pixel_t ins = { powf(fmaxf(0.0f, in[0]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[1]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[2]), 1.0f / HL_POWERF), 0.0f };
  const dt_aligned_pixel_t opp = { 0.5f*(ins[1]+ins[2]), 0.5f*(ins[0]+ins[2]), 0.5f*(ins[0]+ins[1]), 0.0f};

  return powf(opp[color], HL_POWERF);
}

static inline size_t _raw_to_cmap(const size_t width, const size_t row, const size_t col)
{
  return (row / 3) * width + (col / 3);
}

static inline char _mask_dilated(const char *in, const size_t w1)
{
  if(in[0])
    return 1;

  if(in[-w1-1] | in[-w1] | in[-w1+1] | in[-1] | in[1] | in[w1-1] | in[w1] | in[w1+1])
    return 1;

  const size_t w2 = 2*w1;
  const size_t w3 = 3*w1;
  return (in[-w3-2] | in[-w3-1] | in[-w3]   | in[-w3+1] | in[-w3+2] |
          in[-w2-3] | in[-w2-2] | in[-w2-1] | in[-w2]   | in[-w2+1] | in[-w2+2] | in[-w2+3] |
          in[-w1-3] | in[-w1-2] | in[-w1+2] | in[-w1+3] |
          in[-3]    | in[-2]    | in[2]     | in[3]     |
          in[w1-3]  | in[w1-2]  | in[w1+2]  | in[w1+3]  |
          in[w2-3]  | in[w2-2]  | in[w2-1]  | in[w2]    | in[w2+1]  | in[w2+2]  | in[w2+3] |
          in[w3-2]  | in[w3-1]  | in[w3]    | in[w3+1]  | in[w3+2]) ? 1 : 0;
}


// A slightly modified version for sraws
static void _process_linear_opposed(dt_iop_module_t *self,
                                    dt_dev_pixelpipe_iop_t *piece,
                                    const float *const input,
                                    float *const output,
                                    const dt_iop_roi_t *const roi_in,
                                    const dt_iop_roi_t *const roi_out,
                                    const gboolean quality)
{
  dt_iop_highlights_data_t *d = piece->data;
  const float clipval = highlights_clip_magics[DT_IOP_HIGHLIGHTS_OPPOSED] * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};

  const size_t mwidth  = roi_in->width / 3;
  const size_t mheight = roi_in->height / 3;
  const size_t msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 16);

  /* As we don't have linear raws available with full image as roi_in we can't use any
     precalculated chroma correction coeffs
  */

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  char *mask = (quality) ? dt_calloc_align_type(char, 6 * msize) : NULL;
  if(mask)
  {
    gboolean anyclipped = FALSE;
    DT_OMP_FOR(reduction( | : anyclipped))
    for(size_t row = 1; row < roi_in->height -1; row++)
    {
      for(size_t col = 1; col < roi_in->width -1; col++)
      {
        const size_t idx = (row * roi_in->width + col) * 4;
        const size_t mdx = _raw_to_cmap(mwidth, row, col);
        for_three_channels(c)
        {
          if((input[idx] >= clips[c]) && (mask[c*msize + mdx] == 0))
          {
            mask[c * msize + mdx] |= 1;
            anyclipped |= TRUE;
          }
        }
      }
    }
    /* We want to use the photosites closely around clipped data to be taken into account.
       The mask buffers holds data for each color channel, we dilate the mask buffer slightly
       to get those locations.
    */

    dt_aligned_pixel_t sums = {0.0f, 0.0f, 0.0f, 0.0f};
    dt_aligned_pixel_t cnts = {0.0f, 0.0f, 0.0f, 0.0f};

    if(anyclipped)
    {
      DT_OMP_FOR(collapse(2))
      for(size_t row = 3; row < mheight - 3; row++)
      {
        for(size_t col = 3; col < mwidth - 3; col++)
        {
          const size_t mx = row * mwidth + col;
          mask[3*msize + mx] = _mask_dilated(mask + mx, mwidth);
          mask[4*msize + mx] = _mask_dilated(mask + msize + mx, mwidth);
          mask[5*msize + mx] = _mask_dilated(mask + 2*msize + mx, mwidth);
        }
      }

      DT_OMP_FOR(reduction(+ : sums, cnts))
      for(size_t row = 3; row < roi_in->height - 3; row++)
      {
        for(size_t col = 3; col < roi_in->width - 3; col++)
        {
          const size_t idx = (row * roi_in->width + col) * 4;
          for_three_channels(c)
          {
            const float inval = input[idx+c];
            if((inval > 0.2f * clips[c]) && (inval < clips[c]) && (mask[(c+3) * msize + _raw_to_cmap(mwidth, row, col)]))
            {
              sums[c] += inval - _calc_linear_refavg(&input[idx], c);
              cnts[c] += 1.0f;
            }
          }
        }
      }
      for_three_channels(c)
        chrominance[c] = (cnts[c] > 30.0f) ? sums[c] / cnts[c] : 0.0f;
    }
    dt_free_align(mask);
  }
  DT_OMP_FOR(collapse(2))
  for(ssize_t row = 0; row < roi_out->height; row++)
  {
    for(ssize_t col = 0; col < roi_out->width; col++)
    {
      const ssize_t odx = (row * roi_out->width + col) * 4;
      const ssize_t inrow = MIN(row, roi_in->height-1);
      const ssize_t incol = MIN(col, roi_in->width-1);
      const ssize_t idx = (inrow * roi_in->width + incol) * 4;
      for_three_channels(c)
      {
        const float ref = _calc_linear_refavg(&input[idx], c);
        const float inval = fmaxf(0.0f, input[idx+c]);
        output[odx+c] = (inval >= clips[c]) ? fmaxf(inval, ref + chrominance[c]) : inval;
      }
    }
  }
}

static float *_process_opposed(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const float *const input,
                               float *const output,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out,
                               const gboolean keep,
                               const gboolean quality)
{
  dt_iop_highlights_data_t *d = piece->data;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const float clipval = highlights_clip_magics[DT_IOP_HIGHLIGHTS_OPPOSED] * d->clip;

  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};

  const dt_dev_chroma_t *chr = &self->dev->chroma;
  const gboolean late = chr->late_correction;
  const dt_aligned_pixel_t correction = { late ? (float)(chr->D65coeffs[0] / chr->as_shot[0]) : 1.0f,
                                          late ? (float)(chr->D65coeffs[1] / chr->as_shot[1]) : 1.0f,
                                          late ? (float)(chr->D65coeffs[2] / chr->as_shot[2]) : 1.0f,
                                          1.0f };

  const size_t mwidth  = roi_in->width / 3;
  const size_t mheight = roi_in->height / 3;
  const size_t msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 16);

  const dt_hash_t opphash = _opposed_hash(piece);
  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  if(opphash == img_opphash)
  {
    for_three_channels(c)
      chrominance[c] = img_oppchroma[c];
    if(!img_oppclipped && !keep)
    {
      dt_iop_copy_image_roi(output, input, 1, roi_in, roi_out);
      return NULL;
    }
  }
  else
  {
    char *mask = (quality) ? dt_calloc_align_type(char, 6 * msize) : NULL;
    if(mask)
    {
      gboolean anyclipped = FALSE;
      DT_OMP_FOR(reduction( | : anyclipped) collapse(2))
      for(int mrow = 1; mrow < mheight-1; mrow++)
      {
        for(int mcol = 1; mcol < mwidth-1; mcol++)
        {
          char mbuff[3] = { 0, 0, 0 };
          const size_t grp = 3 * (mrow * roi_in->width + mcol);
          for(int y = -1; y < 2; y++)
          {
            for(int x = -1; x < 2; x++)
            {
              const size_t idx = grp + y * roi_in->width + x;
              const int color = (filters == 9u) ? FCxtrans(mrow+y, mcol+x, roi_in, xtrans) : FC(mrow+y, mcol+x, filters);
              const gboolean clipped = input[idx] >= clips[color];
              mbuff[color] += (clipped) ? 1 : 0;
            }
          }
          for_three_channels(c)
          {
            mask[c * msize + mrow * mwidth + mcol] = (mbuff[c]) ? 1 : 0;
            anyclipped |= (mbuff[c]) ? 1 : 0;
          }
        }
      }

      dt_aligned_pixel_t sums = {0.0f, 0.0f, 0.0f, 0.0f};
      dt_aligned_pixel_t cnts = {0.0f, 0.0f, 0.0f, 0.0f};

      if(anyclipped)
      {
        /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
         If there are no clipped locations we keep the chrominance correction at 0 but make it valid
        */
        DT_OMP_FOR(collapse(2))
        for(size_t row = 3; row < mheight - 3; row++)
        {
          for(size_t col = 3; col < mwidth - 3; col++)
          {
            const size_t mx = row * mwidth + col;
            mask[3*msize + mx] = _mask_dilated(mask + mx, mwidth);
            mask[4*msize + mx] = _mask_dilated(mask + msize + mx, mwidth);
            mask[5*msize + mx] = _mask_dilated(mask + 2*msize + mx, mwidth);
          }
        }

        const dt_aligned_pixel_t lo_clips = { 0.2f * clips[0], 0.2f * clips[1], 0.2f * clips[2], 1.0f };
       /* After having the surrounding mask for each color channel we can calculate the chrominance corrections. */
        DT_OMP_FOR(reduction(+ : sums, cnts) collapse(2))
        for(size_t row = 3; row < roi_in->height - 3; row++)
        {
          for(size_t col = 3; col < roi_in->width - 3; col++)
          {
            const size_t idx = row * roi_in->width + col;
            const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
            const float inval = input[idx];

            /* we only use the unclipped photosites very close the true clipped data to calculate the chrominance offset */
            if((inval < clips[color]) && (inval > lo_clips[color])
               && (mask[(color+3) * msize + _raw_to_cmap(mwidth, row, col)]))
            {
              sums[color] += inval - _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, TRUE);
              cnts[color] += 1.0f;
            }
          }
        }
        for_three_channels(c)
          chrominance[c] = (cnts[c] > 100.0f) ? sums[c] / cnts[c] : 0.0f;
      }

      if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
      {
        for_three_channels(c)
          img_oppchroma[c] = chrominance[c];
        img_opphash = opphash;
        img_oppclipped = anyclipped;
      }

      dt_print_pipe(DT_DEBUG_PIPE,
          "opposed chroma", piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
          "RGB %3.4f %3.4f %3.4f hash=%" PRIx64 "%s%s",
          chrominance[0], chrominance[1], chrominance[2],
          _opposed_parhash(piece),
          piece->pipe->type == DT_DEV_PIXELPIPE_FULL ? ", saved" : "",
          img_oppclipped ? "" : ", unclipped");
    }
    dt_free_align(mask);
  }

  float *tmpout = (keep) ? dt_alloc_align_float(roi_in->width * roi_in->height) : NULL;
  if(tmpout)
  {
    DT_OMP_FOR(collapse(2))
    for(size_t row = 0; row < roi_in->height; row++)
    {
      for(size_t col = 0; col < roi_in->width; col++)
      {
        const size_t idx = row * roi_in->width + col;
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float inval = MAX(0.0f, input[idx]);
        if(inval >= clips[color])
        {
          const float ref = _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, TRUE);
          tmpout[idx] = MAX(inval, ref + chrominance[color]);
        }
        else
          tmpout[idx] = inval;
      }
    }
  }

  DT_OMP_FOR(collapse(2))
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
          oval = MAX(0.0f, input[ix]);
          if(oval >= clips[color])
          {
            const float ref = _calc_refavg(input, xtrans, filters, irow, icol, roi_in, correction, TRUE);
            oval = MAX(oval, ref + chrominance[color]);
          }
        }
      }
      output[odx] = oval;
    }
  }
  return tmpout;
}

#ifdef HAVE_OPENCL
static cl_int process_opposed_cl(dt_iop_module_t *self,
                                 dt_dev_pixelpipe_iop_t *piece,
                                 cl_mem dev_in,
                                 cl_mem dev_out,
                                 const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlights_data_t *d = piece->data;
  const dt_iop_highlights_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;
  const uint32_t filters = piece->pipe->dsc.filters;

  const float clipval = highlights_clip_magics[DT_IOP_HIGHLIGHTS_OPPOSED] * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};

  dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2], 1.0f};

  const dt_dev_chroma_t *chr = &self->dev->chroma;
  const gboolean late = chr->late_correction;
  dt_aligned_pixel_t correction = { late ? (float)(chr->D65coeffs[0] / chr->as_shot[0]) : 1.0f,
                                    late ? (float)(chr->D65coeffs[1] / chr->as_shot[1]) : 1.0f,
                                    late ? (float)(chr->D65coeffs[2] / chr->as_shot[2]) : 1.0f,
                                    1.0f };

  cl_int err = DT_OPENCL_SYSMEM_ALLOCATION;
  cl_mem dev_chrominance = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;
  cl_mem dev_inmask = NULL;
  cl_mem dev_outmask = NULL;
  cl_mem dev_accu = NULL;
  cl_mem dev_correction = NULL;
  float *claccu = NULL;

  const size_t iheight = ROUNDUPDHT(roi_in->height, devid);
  const int mwidth  = roi_in->width / 3;
  const int mheight = roi_in->height / 3;
  const int msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 16);

  const dt_hash_t opphash = _opposed_hash(piece);
  const int fastcopymode = (opphash == img_opphash) && !img_oppclipped;

  if(!fastcopymode)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;

    dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
    if(dev_clips == NULL) goto error;

    dev_correction = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), correction);
    if(dev_correction == NULL) goto error;
  }

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  if(opphash == img_opphash)
  {
    for_three_channels(c)
      chrominance[c] = img_oppchroma[c];
  }
  else
  {
    // We don't have valid chrominance correction so go the hard way

    dev_inmask = dt_opencl_alloc_device_buffer(devid, sizeof(char) * 3 * msize);
    if(dev_inmask == NULL) goto error;

    dev_outmask =  dt_opencl_alloc_device_buffer(devid, sizeof(char) * 3 * msize);
    if(dev_outmask == NULL) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_initmask, mwidth, mheight,
            CLARG(dev_in), CLARG(dev_inmask),
            CLARG(msize), CLARG(mwidth), CLARG(mheight),
            CLARG(filters), CLARG(dev_xtrans),
            CLARG(dev_clips));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_dilatemask, mwidth, mheight,
            CLARG(dev_inmask), CLARG(dev_outmask),
            CLARG(msize), CLARG(mwidth), CLARG(mheight));
    if(err != CL_SUCCESS) goto error;

    err = DT_OPENCL_SYSMEM_ALLOCATION;
    const size_t accusize = sizeof(float) * 8 * iheight;
    dev_accu = dt_opencl_alloc_device_buffer(devid, accusize);
    if(dev_accu == NULL) goto error;

    claccu = dt_calloc_align_float(8 * iheight);
    if(claccu == NULL) goto error;

    err = dt_opencl_write_buffer_to_device(devid, claccu, dev_accu, 0, accusize, TRUE);
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_highlights_chroma, iheight,
            CLARG(dev_in), CLARG(dev_outmask), CLARG(dev_accu),
            CLARG(roi_in->width), CLARG(roi_in->height),
            CLARG(msize), CLARG(mwidth),
            CLARG(filters), CLARG(dev_xtrans), CLARG(dev_clips), CLARG(dev_correction));

    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_read_buffer_from_device(devid, claccu, dev_accu, 0, accusize, TRUE);
    if(err != CL_SUCCESS) goto error;

    // collect row data and accumulate
    dt_aligned_pixel_t sums = { 0.0f, 0.0f, 0.0f};
    dt_aligned_pixel_t cnts = { 0.0f, 0.0f, 0.0f};
    float clipped = 0.0f;
    for(int row = 3; row < roi_in->height - 3; row++)
    {
      for_three_channels(c)
      {
        sums[c] += claccu[8*row + 2*c];
        cnts[c] += claccu[8*row + 2*c +1];
        clipped += claccu[8*row + 6];
      }
    }
    for_three_channels(c)
      chrominance[c] = (cnts[c] > 100.0f) ? sums[c] / cnts[c] : 0.0f;

    if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      for_three_channels(c)
        img_oppchroma[c] = chrominance[c];
      img_opphash = opphash;
      img_oppclipped = clipped > 0.0f;
    }

    dt_print_pipe(DT_DEBUG_PIPE,
        "opposed chroma", piece->pipe, self, piece->pipe->devid, roi_in, roi_out,
        "RGB %3.4f %3.4f %3.4f hash=%" PRIx64 "%s%s",
        chrominance[0], chrominance[1], chrominance[2],
        _opposed_parhash(piece),
        piece->pipe->type == DT_DEV_PIXELPIPE_FULL ? ", saved" : "",
        img_oppclipped ? "" : ", unclipped");
  }

  err = DT_OPENCL_SYSMEM_ALLOCATION;
  dev_chrominance = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), chrominance);
  if(dev_chrominance == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_opposed, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(roi_in->width), CLARG(roi_in->height),
          CLARG(roi_out->x), CLARG(roi_out->y),
          CLARG(filters), CLARG(dev_xtrans),
          CLARG(dev_clips),
          CLARG(dev_chrominance),
          CLARG(dev_correction),
          CLARG(fastcopymode));

  error:
  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_chrominance);
  dt_opencl_release_mem_object(dev_inmask);
  dt_opencl_release_mem_object(dev_outmask);
  dt_opencl_release_mem_object(dev_accu);
  dt_opencl_release_mem_object(dev_correction);
  dt_free_align(claccu);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
