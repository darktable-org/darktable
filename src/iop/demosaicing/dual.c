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

static float _slider2contrast(const float slider)
{
  return 0.005f * powf(slider, 1.1f);
}

static void dual_demosaic(dt_dev_pixelpipe_iop_t *piece,
                          float *const restrict high_data,
                          const float *const restrict raw_data,
                          const int width,
                          const int height,
                          const uint32_t filters,
                          const uint8_t (*const xtrans)[6],
                          const gboolean dual_mask,
                          const float dual_threshold)
{
  if(width < 16 || height < 16) return;
  dt_dev_pixelpipe_t *p = piece->pipe;

  // If the threshold is zero and we don't want to see the blend mask we don't do anything
  if(dual_threshold <= 0.0f && !dual_mask) return;

  const size_t msize = (size_t)width * height;
  const float contrastf = _slider2contrast(dual_threshold);

  float *mask = dt_masks_calc_scharr_mask(p, high_data, width, height, TRUE);
  float *tmp = dt_iop_image_alloc(width, height, 1);
  if(!mask || !tmp)
  {
    dt_free_align(mask);
    dt_free_align(tmp);
    return;
  }
  dt_masks_calc_detail_blend(mask, tmp, msize, contrastf, TRUE);
  dt_gaussian_fast_blur(tmp, mask, width, height, 2.0f, 0.0f, 1.0f, 1);

  if(dual_mask)
  {
    DT_OMP_FOR_SIMD(aligned(mask : 64))
    for(size_t idx = 0; idx < msize; idx++)
      high_data[idx * 4 + 3] = mask[idx];
  }
  else
  {
    float *vng_image = dt_iop_image_alloc(width, height, 4);
    if(vng_image)
    {
      vng_interpolate(vng_image, raw_data, width, height, filters, xtrans, TRUE);
      color_smoothing(vng_image, width, height, DT_DEMOSAIC_SMOOTH_2);

      DT_OMP_FOR_SIMD(aligned(mask, vng_image : 64))
      for(size_t idx = 0; idx < msize; idx++)
      {
        const size_t oidx = idx * 4;
        for(int c = 0; c < 3; c++)
          high_data[oidx + c] = interpolatef(mask[idx], high_data[oidx + c], vng_image[oidx + c]);
        high_data[oidx + 3] = 0.0f;
      }
      dt_free_align(vng_image);
    }
  }
  dt_free_align(mask);
  dt_free_align(tmp);
}

#ifdef HAVE_OPENCL
int dual_demosaic_cl(const dt_iop_module_t *self,
                     const dt_dev_pixelpipe_iop_t *const piece,
                     cl_mem high_image,
                     cl_mem low_image,
                     cl_mem out,
                     const int width,
                     const int height,
                     const int dual_mask)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  const int devid = p->devid;

  dt_iop_demosaic_data_t *data = piece->data;
  const dt_iop_demosaic_global_data_t *gd = self->global_data;

  const float contrastf = _slider2contrast(data->dual_thrs);

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  const size_t bsize = sizeof(float) * width * height;

  cl_mem tmp = dt_opencl_alloc_device_buffer(devid, bsize);
  cl_mem mask = dt_opencl_alloc_device_buffer(devid, bsize);
  if(!mask || !tmp) goto finish;

  const gboolean wboff = !p->dsc.temperature.enabled;
  const dt_aligned_pixel_t wb =
      { wboff ? 1.0f : p->dsc.temperature.coeffs[0],
        wboff ? 1.0f : p->dsc.temperature.coeffs[1],
        wboff ? 1.0f : p->dsc.temperature.coeffs[2] };

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_Y0_mask, width, height,
     CLARG(mask), CLARG(high_image), CLARG(width), CLARG(height),
     CLARG(wb[0]), CLARG(wb[1]), CLARG(wb[2]));
  if(err != CL_SUCCESS) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_scharr_mask, width, height,
     CLARG(mask), CLARG(tmp), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS) goto finish;

  const int detail = 1;
  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_blend, width, height,
      CLARG(tmp), CLARG(mask), CLARG(width), CLARG(height), CLARG(contrastf), CLARG(detail));
  if(err != CL_SUCCESS) goto finish;

  err = dt_gaussian_fast_blur_cl_buffer(devid, mask, tmp, width, height, 2.0f, 1, 0.0f, 1.0f);
  if(err != CL_SUCCESS) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_write_blended_dual, width, height,
      CLARG(high_image), CLARG(low_image), CLARG(out), CLARG(width), CLARG(height), CLARG(tmp), CLARG(dual_mask));

  finish:

  if(err != CL_SUCCESS)
    dt_print_pipe(DT_DEBUG_ALWAYS, "dual demosaic", p, self, devid, NULL, NULL, "Error: %s", cl_errstr(err));

  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(tmp);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
