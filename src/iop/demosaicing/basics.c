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

#define SWAP(a, b)                                                                                           \
  {                                                                                                          \
    const float tmp = (b);                                                                                   \
    (b) = (a);                                                                                               \
    (a) = tmp;                                                                                               \
  }

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void pre_median_b(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi,
        const uint32_t filters,
        const int num_passes,
        const float threshold)
{
  dt_iop_image_copy_by_size(out, in, roi->width, roi->height, 1);

  // now green:
  const int lim[5] = { 0, 1, 2, 1, 0 };
  for(int pass = 0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(filters, in, lim, roi, threshold) \
    shared(out) \
    schedule(static)
#endif
    for(int row = 3; row < roi->height - 3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row, col, filters) != 1 && FC(row, col, filters) != 3) col++;
      float *pixo = out + (size_t)roi->width * row + col;
      const float *pixi = in + (size_t)roi->width * row + col;
      for(; col < roi->width - 3; col += 2)
      {
        int cnt = 0;
        for(int k = 0, i = 0; i < 5; i++)
        {
          for(int j = -lim[i]; j <= lim[i]; j += 2)
          {
            if(fabsf(pixi[roi->width * (i - 2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[roi->width * (i - 2) + j];
              cnt++;
            }
            else
              med[k++] = 64.0f + pixi[roi->width * (i - 2) + j];
          }
        }
        for(int i = 0; i < 8; i++)
          for(int ii = i + 1; ii < 9; ii++)
            if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = fmaxf(0.0f, (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]));
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void pre_median(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi,
        const uint32_t filters,
        const int num_passes,
        const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I, J)                                                                                        \
  if(med[I] > med[J]) SWAP(med[I], med[J])

static void color_smoothing(
        float *out,
        const dt_iop_roi_t *const roi_out,
        const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      {
        float *outp = out;
        for(int j = 0; j < roi_out->height; j++)
          for(int i = 0; i < roi_out->width; i++, outp += 4) outp[3] = outp[c];
      }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(roi_out, width4) \
      shared(out, c) \
      schedule(static)
#endif
      for(int j = 1; j < roi_out->height - 1; j++)
      {
        float *outp = out + (size_t)4 * j * roi_out->width + 4;
        for(int i = 1; i < roi_out->width - 1; i++, outp += 4)
        {
          float med[9] = {
            outp[-width4 - 4 + 3] - outp[-width4 - 4 + 1], outp[-width4 + 0 + 3] - outp[-width4 + 0 + 1],
            outp[-width4 + 4 + 3] - outp[-width4 + 4 + 1], outp[-4 + 3] - outp[-4 + 1],
            outp[+0 + 3] - outp[+0 + 1], outp[+4 + 3] - outp[+4 + 1],
            outp[+width4 - 4 + 3] - outp[+width4 - 4 + 1], outp[+width4 + 0 + 3] - outp[+width4 + 0 + 1],
            outp[+width4 + 4 + 3] - outp[+width4 + 4 + 1],
          };
          /* optimal 9-element median search */
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 1);
          SWAPmed(3, 4);
          SWAPmed(6, 7);
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 3);
          SWAPmed(5, 8);
          SWAPmed(4, 7);
          SWAPmed(3, 6);
          SWAPmed(1, 4);
          SWAPmed(2, 5);
          SWAPmed(4, 7);
          SWAPmed(4, 2);
          SWAPmed(6, 4);
          SWAPmed(4, 2);
          outp[c] = fmaxf(med[4] + outp[1], 0.0f);
        }
      }
    }
  }
}
#undef SWAP

static void green_equilibration_lavg(
        float *out,
        const float *const in,
        const int width,
        const int height,
        const uint32_t filters,
        const int x,
        const int y,
        const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj + y, oi + x, filters) != 1) oj++;
  if(FC(oj + y, oi + x, filters) != 1) oi++;
  if(FC(oj + y, oi + x, filters) != 1) oj--;

  dt_iop_image_copy_by_size(out, in, width, height, 1);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, in, thr, width, maximum) \
  shared(out, oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < height - 2; j += 2)
  {
    for(size_t i = oi; i < width - 2; i += 2)
    {
      const float o1_1 = in[(j - 1) * width + i - 1];
      const float o1_2 = in[(j - 1) * width + i + 1];
      const float o1_3 = in[(j + 1) * width + i - 1];
      const float o1_4 = in[(j + 1) * width + i + 1];
      const float o2_1 = in[(j - 2) * width + i];
      const float o2_2 = in[(j + 2) * width + i];
      const float o2_3 = in[j * width + i - 2];
      const float o2_4 = in[j * width + i + 2];

      const float m1 = (o1_1 + o1_2 + o1_3 + o1_4) / 4.0f;
      const float m2 = (o2_1 + o2_2 + o2_3 + o2_4) / 4.0f;

      // prevent divide by zero and ...
      // guard against m1/m2 becoming too large (due to m2 being too small) which results in hot pixels
      // also m1 must be checked to be positive
      if((m2 > 0.0f) && (m1 > 0.0f) && (m1 / m2 < maximum * 2.0f))
      {
        const float c1 = (fabsf(o1_1 - o1_2) + fabsf(o1_1 - o1_3) + fabsf(o1_1 - o1_4) + fabsf(o1_2 - o1_3)
                          + fabsf(o1_3 - o1_4) + fabsf(o1_2 - o1_4)) / 6.0f;
        const float c2 = (fabsf(o2_1 - o2_2) + fabsf(o2_1 - o2_3) + fabsf(o2_1 - o2_4) + fabsf(o2_2 - o2_3)
                          + fabsf(o2_3 - o2_4) + fabsf(o2_2 - o2_4)) / 6.0f;
        if((in[j * width + i] < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
        {
          out[j * width + i] = fmaxf(0.0f, in[j * width + i] * m1 / m2);
        }
      }
    }
  }
}

static void green_equilibration_favg(
        float *out,
        const float *const in,
        const int width,
        const int height,
        const uint32_t filters,
        const int x,
        const int y)
{
  int oj = 0, oi = 0;
  // const float ratio_max = 1.1f;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if((FC(oj + y, oi + x, filters) & 1) != 1) oi++;
  const int g2_offset = oi ? -1 : 1;
  dt_iop_image_copy_by_size(out, in, width, height, 1);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  reduction(+ : sum1, sum2) \
  shared(oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < (height - 1); j += 2)
  {
    for(size_t i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      sum1 += in[j * width + i];
      sum2 += in[(j + 1) * width + i + g2_offset];
    }
  }

  if(sum1 > 0.0 && sum2 > 0.0)
    gr_ratio = sum2 / sum1;
  else
    return;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  shared(out, oi, oj, gr_ratio) \
  schedule(static) collapse(2)
#endif
  for(int j = oj; j < (height - 1); j += 2)
  {
    for(int i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      out[(size_t)j * width + i] = fmaxf(0.0f, in[(size_t)j * width + i] * gr_ratio);
    }
  }
}

#ifdef HAVE_OPENCL
// color smoothing step by multiple passes of median filtering
static int color_smoothing_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_out,
        const int passes)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float), .overhead = 0,
                                  .sizex = 1 << 8, .sizey = 1 << 8 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_color_smoothing, &locopt))
    goto error;

  // two buffer references for our ping-pong
  cl_mem dev_t1 = dev_out;
  cl_mem dev_t2 = dev_tmp;

  for(int pass = 0; pass < passes; pass++)
  {
    size_t sizes[] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_color_smoothing, 0, CLARG(dev_t1), CLARG(dev_t2), CLARG(width),
      CLARG(height), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_color_smoothing, sizes, local);
    if(err != CL_SUCCESS) goto error;

    // swap dev_t1 and dev_t2
    cl_mem t = dev_t1;
    dev_t1 = dev_t2;
    dev_t2 = t;
  }

  // after last step we find final output in dev_t1.
  // let's see if this is in dev_tmp1 and needs to be copied to dev_out
  if(dev_t1 == dev_tmp)
  {
    // copy data from dev_tmp -> dev_out
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_color_smoothing] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int green_equilibration_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  cl_mem dev_in1 = NULL;
  cl_mem dev_out1 = NULL;
  cl_mem dev_in2 = NULL;
  cl_mem dev_out2 = NULL;
  float *sumsum = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(dev_tmp == NULL) goto error;
  }

  switch(data->green_eq)
  {
    case DT_IOP_GREEN_EQ_FULL:
      dev_in1 = dev_in;
      dev_out1 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_LOCAL:
      dev_in2 = dev_in;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_BOTH:
      dev_in1 = dev_in;
      dev_out1 = dev_tmp;
      dev_in2 = dev_tmp;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_NO:
    default:
      goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_FULL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dt_opencl_local_buffer_t flocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 2 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 4, .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_first, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);

    const int bufsize = (bwidth / flocopt.sizex) * (bheight / flocopt.sizey);

    dev_m = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * bufsize);
    if(dev_m == NULL) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_favg_reduce_first, 0, CLARG(dev_in1), CLARG(width),
      CLARG(height), CLARG(dev_m), CLARG(piece->pipe->dsc.filters), CLARG(roi_in->x), CLARG(roi_in->y),
      CLLOCAL(sizeof(float) * 2 * flocopt.sizex * flocopt.sizey));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_first, fsizes,
                                                 flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = sizeof(float) * 2, .overhead = 0,
                                    .sizex = 1 << 16, .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_second, &slocopt))
      goto error;

    const int reducesize = MIN(DT_REDUCESIZE_MIN, ROUNDUP(bufsize, slocopt.sizex) / slocopt.sizex);

    dev_r = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * reducesize);
    if(dev_r == NULL) goto error;

    size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
    size_t slocal[3] = { slocopt.sizex, 1, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_favg_reduce_second, 0, CLARG(dev_m), CLARG(dev_r),
      CLARG(bufsize), CLLOCAL(sizeof(float) * 2 * slocopt.sizex));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_second, ssizes,
                                                 slocal);
    if(err != CL_SUCCESS) goto error;

    sumsum = dt_alloc_align_float((size_t)2 * reducesize);
    if(sumsum == NULL) goto error;
    err = dt_opencl_read_buffer_from_device(devid, (void *)sumsum, dev_r, 0,
                                            sizeof(float) * 2 * reducesize, CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    float sum1 = 0.0f, sum2 = 0.0f;
    for(int k = 0; k < reducesize; k++)
    {
      sum1 += sumsum[2 * k];
      sum2 += sumsum[2 * k + 1];
    }

    const float gr_ratio = (sum1 > 0.0f && sum2 > 0.0f) ? sum2 / sum1 : 1.0f;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_green_eq_favg_apply, width, height,
      CLARG(dev_in1), CLARG(dev_out1), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters),
      CLARG(roi_in->x), CLARG(roi_in->y), CLARG(gr_ratio));
    if(err != CL_SUCCESS) goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_LOCAL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    const dt_image_t *img = &self->dev->image_storage;
    const float threshold = 0.0001f * img->exif_iso;

    dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_lavg, &locopt))
      goto error;

    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_lavg, 0, CLARG(dev_in2), CLARG(dev_out2),
      CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(roi_in->x), CLARG(roi_in->y),
      CLARG(threshold), CLLOCAL(sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_lavg, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_free_align(sumsum);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_free_align(sumsum);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_green_equilibration] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int process_default_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const int demosaicing_method)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const int devid = piece->pipe->devid;
  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  cl_mem dev_aux = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_med = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;

    // green equilibration
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in))
        goto error;

      dev_in = dev_green_eq;
    }

    // need to reserve scaled auxiliary buffer or use dev_out
    if(scaled)
    {
      dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_aux == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }
    else
      dev_aux = dev_out;

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_passthrough_monochrome, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    {
      cl_mem dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
      if(dev_xtrans == NULL) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_passthrough_color, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(piece->pipe->dsc.filters), CLARG(dev_xtrans));
      dt_opencl_release_mem_object(dev_xtrans);
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PPG)
    {
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_tmp == NULL) goto error;

      {
        const int myborder = 3;
        // manage borders
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_border_interpolate, width, height,
          CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(myborder));
        if(err != CL_SUCCESS) goto error;
      }

      if(data->median_thrs > 0.0f)
      {
        dev_med = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
        if(dev_med == NULL) goto error;

        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                        .cellsize = 1 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pre_median, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_pre_median, 0, CLARG(dev_in), CLARG(dev_med), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(data->median_thrs), CLLOCAL(sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pre_median, sizes, local);
        if(err != CL_SUCCESS) goto error;
        dev_in = dev_aux;
      }
      else dev_med = dev_in;

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                        .cellsize = sizeof(float) * 1, .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_ppg_green, 0, CLARG(dev_med), CLARG(dev_tmp), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3)));

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_green, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                        .cellsize = 4 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_ppg_redblue, 0, CLARG(dev_tmp), CLARG(dev_aux), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)));

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_redblue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }

    if(piece->pipe->want_detail_mask)
      dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, TRUE);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self, roi_in, roi_out, "\n");
      // scale aux buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      // sample image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_passthrough_monochrome, width, height,
        CLARG(dev_pix), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // sample half-size image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, width, height,
        CLARG(dev_pix), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dev_aux = dev_green_eq = dev_tmp = dev_med = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

