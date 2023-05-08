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

/*
   Dual demosaicing has been implemented by Ingo Weyrich <heckflosse67@gmx.de> for
   rawtherapee under GNU General Public License Version 3
   and has been modified to work for darktable by Hanno Schwalm (hanno@schwalm-bremen.de).
   Also the code for dt_masks_blur_9x9 has been taken from rawtherapee capturesharpening,
   implemented also by Ingo Weyrich.
*/

static float slider2contrast(float slider)
{
  return 0.005f * powf(slider, 1.1f);
}
static void dual_demosaic(
        dt_dev_pixelpipe_iop_t *piece,
        float *const restrict rgb_data,
        const float *const restrict raw_data,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6],
        const gboolean dual_mask,
        const float dual_threshold)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  if((width < 16) || (height < 16)) return;

  // If the threshold is zero and we don't want to see the blend mask we don't do anything
  if(dual_threshold <= 0.0f) return;

  float *blend = dt_alloc_align_float((size_t) width * height);
  float *vng_image = dt_alloc_align_float((size_t) 4 * width * height);
  if(!blend || !vng_image)
  {
    if(blend) dt_free_align(blend);
    if(vng_image) dt_free_align(vng_image);
    dt_control_log(_("[dual demosaic] can't allocate internal buffers"));
    return;
  }

  const float contrastf = slider2contrast(dual_threshold);
  if(dt_masks_calc_detail_mask(&piece->pipe->details, blend, contrastf, TRUE))
    return;

  if(dual_mask)
  {
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, width, height) \
  schedule(simd:static) aligned(blend, rgb_data : 64)
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      for(int c = 0; c < 4; c++)
        rgb_data[idx * 4 + c] = blend[idx];
    }
  }
  else
  {
    vng_interpolate(vng_image, raw_data, roi_out, roi_in, filters, xtrans, FALSE);
    color_smoothing(vng_image, roi_out, 2);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, vng_image, width, height) \
  schedule(simd:static) aligned(blend, vng_image, rgb_data : 64)
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      const int oidx = 4 * idx;
      for(int c = 0; c < 4; c++)
        rgb_data[oidx + c] = interpolatef(blend[idx], rgb_data[oidx + c], vng_image[oidx + c]);
    }
  }
  dt_free_align(blend);
  dt_free_align(vng_image);
}

#ifdef HAVE_OPENCL
gboolean dual_demosaic_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem high_image,
        cl_mem low_image,
        cl_mem out,
        const dt_iop_roi_t *const roi_in,
        const int showmask)
{
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const float contrastf = slider2contrast(data->dual_thrs);
  if(showmask)
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;

  cl_int err = CL_SUCCESS;
  cl_mem dev_blurmat = NULL;
  cl_mem mask = NULL;
  cl_mem scharr = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  cl_mem tmp = dt_opencl_alloc_device_buffer(devid, width * height * sizeof(float));

  err = dt_opencl_write_host_to_device(devid, piece->pipe->details.data, scharr, width, height, sizeof(float));
  if(err != CL_SUCCESS) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_read_mask, width, height,
      CLARG(tmp), CLARG(scharr), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS) goto finish;

  dt_opencl_release_mem_object(scharr);
  scharr = NULL;

  mask = dt_opencl_alloc_device_buffer(devid, width * height * sizeof(float));
  const int flag = 1;
  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_blend, width, height,
      CLARG(tmp), CLARG(mask), CLARG(width), CLARG(height), CLARG(contrastf), CLARG(flag));
  if(err != CL_SUCCESS) goto finish;

  float blurmat[13];
  dt_masks_blur_9x9_coeff(blurmat, 2.0f);
  dev_blurmat = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 13, blurmat);

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_mask_blur, width, height,
      CLARG(mask), CLARG(tmp), CLARG(width), CLARG(height), CLARG(dev_blurmat));
  if(err != CL_SUCCESS) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_write_blended_dual, width, height,
      CLARG(high_image), CLARG(low_image), CLARG(out), CLARG(width), CLARG(height), CLARG(tmp), CLARG(showmask));

  finish:
  dt_opencl_release_mem_object(scharr);
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(dev_blurmat);
  return (err == CL_SUCCESS);
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

