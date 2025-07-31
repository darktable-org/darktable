/*
    This file is part of darktable,
    Copyright (C) 2010-2025 darktable developers.

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
   Dual demosaicing has been implemented by Ingo Weyrich <heckflosse67@gmx.de> for
   rawtherapee under GNU General Public License Version 3
   and has been modified to work for darktable by Hanno Schwalm (hanno@schwalm-bremen.de).
*/

static float slider2contrast(float slider)
{
  return 0.005f * powf(slider, 1.1f);
}

static void dual_demosaic(const dt_iop_module_t *self,
                          dt_dev_pixelpipe_iop_t *piece,
                          float *const restrict high_data,
                          const float *const restrict raw_data,
                          const dt_iop_roi_t *const roi,
                          const uint32_t filters,
                          const uint8_t (*const xtrans)[6],
                          const gboolean dual_mask)
{
  const int width = roi->width;
  const int height = roi->height;
  if(width < 16 || height < 16) return;

  const dt_iop_demosaic_global_data_t *gd = self->global_data;
  dt_iop_demosaic_data_t *d = piece->data;

  // If the threshold is zero and we don't want to see the blend mask we don't do anything
  if(d->dual_thrs <= 0.0f && !dual_mask) return;

  const float contrastf = slider2contrast(d->dual_thrs);

  // get the unblurred mask and do it here instead for less mem pressure
  float *mask = dt_masks_calc_detail_mask(piece, contrastf, TRUE, FALSE);
  if(!mask) return;

  const float *ckern = gd->gauss_coeffs + _sigma_to_index(d->dual_sigma) * CAPTURE_KERNEL_ALIGN;
  const float *gkern = gd->gauss_coeffs + _sigma_to_index(2.0f) * CAPTURE_KERNEL_ALIGN;

  DT_OMP_FOR()
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
      dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
      float blurr = 0.0f;
      for(int dy = -4; dy < 5; dy++)
      {
        for(int dx = -4; dx < 5; dx++)
        {
          const int x = col + dx;
          const int y = row + dy;
          if(x >= 0 && y >= 0 && x < width && y < height)
          {
            const size_t idx = (size_t)width*y + x;
            const int kdx = 5 * ABS(dy) + ABS(dx);
            const int color = (filters == 9u) ? FCxtrans(y, x, roi, xtrans) : FC(y, x, filters);
            const float weight = ckern[kdx];
            sum[color] += MAX(0.0f, weight * raw_data[idx]);
            cnt[color] += weight;
            blurr += gkern[kdx] * mask[idx];
          }
        }
      }
      const size_t k = ((size_t)width*row + col) * 4;
      for_three_channels(c)
        high_data[k+c] = interpolatef(CLIP(blurr), high_data[k+c], sum[c] / cnt[c]);
      high_data[k+3] = dual_mask ? CLIP(blurr) : 0.0f;
    }
  }
  dt_free_align(mask);
}

#ifdef HAVE_OPENCL
int dual_demosaic_cl(const dt_iop_module_t *self,
                     const dt_dev_pixelpipe_iop_t *piece,
                     cl_mem high_image,
                     cl_mem in_image,
                     cl_mem out_image,
                     const dt_iop_roi_t *const roi,
                     const int dual_mask)
{
  const int devid = piece->pipe->devid;
  const int width = roi->width;
  const int height = roi->height;

  dt_iop_demosaic_data_t *d = piece->data;
  if(width < 16 || height < 16 || (d->dual_thrs <= 0.0f && !dual_mask))
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    return dt_opencl_enqueue_copy_image(devid, high_image, out_image, origin, origin, region);
  }

  const dt_iop_demosaic_global_data_t *gd = self->global_data;
  const float contrastf = slider2contrast(d->dual_thrs);

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  const size_t bsize = sizeof(float) * width * height;

  cl_mem tmp = dt_opencl_copy_host_to_device_constant(devid, bsize, piece->pipe->scharr.data);
  cl_mem mask = dt_opencl_alloc_device_buffer(devid, bsize);
  cl_mem mcoeffs = NULL;
  cl_mem ccoeffs = NULL;
  cl_mem xtrans = NULL;
  if(!mask || !tmp) goto finish;

  const int detail = 1;
  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_blend, width, height,
      CLARG(tmp), CLARG(mask), CLARG(width), CLARG(height), CLARG(contrastf), CLARG(detail));
  dt_opencl_release_mem_object(tmp);
  tmp = NULL;
  if(err != CL_SUCCESS) goto finish;

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  mcoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CAPTURE_KERNEL_ALIGN, gd->gauss_coeffs + _sigma_to_index(2.0f) * CAPTURE_KERNEL_ALIGN);
  ccoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CAPTURE_KERNEL_ALIGN, gd->gauss_coeffs + _sigma_to_index(d->dual_sigma) * CAPTURE_KERNEL_ALIGN);
  if(!mcoeffs || !ccoeffs) goto finish;

  if(self->dev->image_storage.buf_dsc.filters == 9u)
  {
    xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(!xtrans) goto finish;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_write_blended_dual, width, height,
      CLARG(high_image), CLARG(in_image), CLARG(out_image),
      CLARG(width), CLARG(height), CLARG(roi->x), CLARG(roi->y),
      CLARG(piece->pipe->dsc.filters), CLARG(xtrans),
      CLARG(mask), CLARG(mcoeffs), CLARG(ccoeffs), CLARG(dual_mask));

  finish:
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(ccoeffs);
  dt_opencl_release_mem_object(mcoeffs);
  dt_opencl_release_mem_object(xtrans);

  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
