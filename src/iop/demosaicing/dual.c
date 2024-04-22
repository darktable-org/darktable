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
        float *const restrict high_data,
        const float *const restrict raw_data,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6],
        const gboolean dual_mask,
        const float dual_threshold)
{
  if((roi_in->width < 16) || (roi_in->height < 16)) return;

  // If the threshold is zero and we don't want to see the blend mask we don't do anything
  if(dual_threshold <= 0.0f) return;

  const size_t msize = roi_in->width * roi_in->height;
  float *vng_image = NULL;

  const float contrastf = slider2contrast(dual_threshold);

  float *mask = dt_masks_calc_detail_mask(piece, contrastf, TRUE);
  if(!mask) goto error;

  if(dual_mask)
  {
    DT_OMP_FOR_SIMD(aligned(mask, high_data : 64))
    for(int idx = 0; idx < msize; idx++)
      high_data[idx * 4 + 3] = mask[idx];
  }
  else
  {
    vng_image = dt_alloc_align_float((size_t) 4 * msize);
    if(!vng_image) goto error;

    vng_interpolate(vng_image, raw_data, roi_out, roi_in, filters, xtrans, FALSE);
    color_smoothing(vng_image, roi_out, 2);

    DT_OMP_FOR_SIMD(aligned(mask, vng_image, high_data : 64))
    for(int idx = 0; idx < msize; idx++)
    {
      const int oidx = 4 * idx;
      for(int c = 0; c < 3; c++)
        high_data[oidx + c] = interpolatef(mask[idx], high_data[oidx + c], vng_image[oidx + c]);
      high_data[oidx + 3] = 0.0f;
    }
  }

  error:

  dt_free_align(mask);
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
        const int dual_mask)
{
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int clwidth = ROUNDUPDWD(width, devid);
  const int clheight = ROUNDUPDHT(height, devid);

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const float contrastf = slider2contrast(data->dual_thrs);

  cl_int err = CL_SUCCESS;
  cl_mem dev_blurmat = NULL;
  cl_mem mask = dt_opencl_alloc_device_buffer(devid, width * height * sizeof(float));
  cl_mem tmp = dt_opencl_alloc_device_buffer(devid, width * height * sizeof(float));

  err = dt_opencl_write_buffer_to_device(devid, piece->pipe->scharr.data, tmp, 0, sizeof(float) * width * height, TRUE);
  if(err != CL_SUCCESS) goto finish;

  const int flag = 1;
  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_calc_blend, clwidth, clheight,
      CLARG(tmp), CLARG(mask), CLARG(width), CLARG(height), CLARG(contrastf), CLARG(flag));
  if(err != CL_SUCCESS) goto finish;

  float blurmat[13];
  dt_masks_blur_coeff(blurmat, 2.0f);
  dev_blurmat = dt_opencl_copy_host_to_device_constant(devid, sizeof(blurmat), blurmat);

  err = dt_opencl_enqueue_kernel_2d_args(devid, darktable.opencl->blendop->kernel_mask_blur, clwidth, clheight,
      CLARG(mask), CLARG(tmp), CLARG(width), CLARG(height), CLARG(dev_blurmat));
  if(err != CL_SUCCESS) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_write_blended_dual, clwidth, clheight,
      CLARG(high_image), CLARG(low_image), CLARG(out), CLARG(width), CLARG(height), CLARG(tmp), CLARG(dual_mask));

  finish:
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(dev_blurmat);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
