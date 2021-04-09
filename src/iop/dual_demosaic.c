/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

// dual_demosaic is always called **after** the high-frequency demosaicer (rcd, amaze or one of the non-bayer demosaicers)
// and expects the data available in rgb_data as rgba quadruples. 
static void dual_demosaic(dt_dev_pixelpipe_iop_t *piece, float *const restrict rgb_data, const float *const restrict raw_data,
                          dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in, const uint32_t filters, const uint8_t (*const xtrans)[6],
                          const gboolean dual_mask, float dual_threshold)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  if((width < 16) || (height < 16)) return;

  // If the threshold is zero and we don't want to see the blend mask we don't do anything
  if(dual_threshold <= 0.0f) return;

  float *blend = dt_alloc_align_float((size_t) width * height);
  float *tmp = dt_alloc_align_float((size_t) width * height);
  float *vng_image = dt_alloc_align_float((size_t) 4 * width * height);
  if(!blend || !tmp || !vng_image)
  {
    if(tmp) dt_free_align(tmp);
    if(blend) dt_free_align(blend);
    if(vng_image) dt_free_align(vng_image);
    dt_control_log(_("[dual demosaic] can't allocate internal buffers"));
    return;
  }
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF)) && (piece->pipe->type == DT_DEV_PIXELPIPE_FULL));

  vng_interpolate(vng_image, raw_data, roi_out, roi_in, filters, xtrans, DEMOSAIC_FULL_SCALE);

  dt_times_t start_blend = { 0 }, end_blend = { 0 };
  if(info) dt_get_times(&start_blend);

  const float contrastf = dual_threshold / 100.0f;

  dt_masks_calc_luminance_mask(rgb_data, blend, width, height);
  dt_masks_calc_detail_mask(blend, blend, tmp, width, height, contrastf, TRUE);  

  const float filler = 0.0f;
  dt_iop_image_fill(blend, filler, width, 4, 1);
  dt_iop_image_fill(&blend[(height-4) * width], filler, width, 4, 1);
  for(int row = 4; row < height - 4; row++)
  {
    dt_iop_image_fill(&blend[row * width], filler, 4, 1, 1);
    dt_iop_image_fill(&blend[row * width + width - 4], filler, 4, 1, 1);
  }

  if(dual_mask)
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, vng_image, width, height) \
  schedule(simd:static) aligned(blend, vng_image, rgb_data : 64) 
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      for(int c = 0; c < 4; c++)
        rgb_data[idx * 4 + c] = blend[idx];
    }
    dt_iop_image_fill(rgb_data, filler, width, 4, 4);
    dt_iop_image_fill(&rgb_data[4 * ((height-4) * width)], filler, width, 4, 4);
    for(int row = 4; row < height - 4; row++)
    {
      dt_iop_image_fill(&rgb_data[4 * row * width], filler, 4, 1, 4);
      dt_iop_image_fill(&rgb_data[4 * (row * width + width - 4)], filler, 4, 1, 4);
    }
  }
  else
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, vng_image, width, height) \
  schedule(simd:static) aligned(blend, vng_image, rgb_data : 64) 
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      const int oidx = 4 * idx;
      for(int c = 0; c < 4; c++)
        rgb_data[oidx + c] = intp(blend[idx], rgb_data[oidx + c], vng_image[oidx + c]);
    }
  }
  if(info)
  {
    dt_get_times(&end_blend);
    fprintf(stderr," [demosaic] CPU dual blending %.4f secs (%.4f CPU)\n", end_blend.clock - start_blend.clock, end_blend.user - start_blend.user);
  }
  dt_free_align(tmp);
  dt_free_align(blend);
  dt_free_align(vng_image);
}

#ifdef HAVE_OPENCL
gboolean dual_demosaic_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem luminance, cl_mem blend, cl_mem high_image, cl_mem low_image, cl_mem out, const int width, const int height, const int showmask)
{
  const int devid = piece->pipe->devid;
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const float dual_threshold = data->dual_thrs;
  const float contrastf = dual_threshold / 100.0f;

  {
    size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_luminance_mask, 0, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_luminance_mask, 1, sizeof(cl_mem), &high_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_luminance_mask, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_luminance_mask, 3, sizeof(int), &height);
    const int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_calc_luminance_mask, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }  

  {
    const int detail = 1;
    size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 0, sizeof(cl_mem), &luminance);  
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 1, sizeof(cl_mem), &blend);  
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 4, sizeof(float), &contrastf);
    dt_opencl_set_kernel_arg(devid, gd->kernel_calc_detail_blend, 5, sizeof(int), &detail);
    const int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_calc_detail_blend, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }

  {
    // For a blurring sigma of 2.0f a 13x13 kernel would be optimally required but the 9x9 is by far good enough here 
    float kernel[9][9];
    const double temp = -2.0f * (2.0f * 2.0f);
    float sum = 0.0f;
    for(int i = -4; i <= 4; i++)
    {
      for(int j = -4; j <= 4; j++)
      {
        kernel[i + 4][j + 4] = expf(((i*i) + (j*j)) / temp);
        sum += kernel[i + 4][j + 4];
      }
    }
    for(int i = 0; i < 9; i++)
    {
      for(int j = 0; j < 9; j++)
        kernel[i][j] /= sum;
    }
    const float c42 = kernel[0][2];
    const float c41 = kernel[0][3];
    const float c40 = kernel[0][4];
    const float c33 = kernel[1][1];
    const float c32 = kernel[1][2];
    const float c31 = kernel[1][3];
    const float c30 = kernel[1][4];
    const float c22 = kernel[2][2];
    const float c21 = kernel[2][3];
    const float c20 = kernel[2][4];
    const float c11 = kernel[3][3];
    const float c10 = kernel[3][4];
    const float c00 = kernel[4][4];

    size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 0, sizeof(cl_mem), &blend);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 1, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 4, sizeof(int), &c42);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 5, sizeof(int), &c41);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 6, sizeof(int), &c40);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 7, sizeof(int), &c33);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 8, sizeof(int), &c32);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 9, sizeof(int), &c31);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 10, sizeof(int), &c30);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 11, sizeof(int), &c22);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 12, sizeof(int), &c21);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 13, sizeof(int), &c20);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 14, sizeof(int), &c11);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 15, sizeof(int), &c10);
    dt_opencl_set_kernel_arg(devid, gd->kernel_fastblur_mask_9x9, 16, sizeof(int), &c00);
    const int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_fastblur_mask_9x9, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }

  {
    size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 0, sizeof(cl_mem), &high_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 1, sizeof(cl_mem), &low_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 2, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 4, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 5, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, gd->kernel_write_blended_dual, 6, sizeof(int), &showmask);
    const int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_write_blended_dual, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }

  return TRUE;
}
#endif

