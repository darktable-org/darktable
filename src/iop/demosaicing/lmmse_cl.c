/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#ifdef HAVE_OPENCL

// lmmse_demosaic() on the OpenCL device, see data/kernels/demosaic_lmmse.cl.
// PPG fills the 4 pixel border as on the CPU. The six buffers cover the
// image plus that border; every pass writes all of its buffer, zeros where
// the CPU tile has no data. The color planes and the median corrections
// reuse the buffers whose contents are no longer read, as the CPU tile
// buffers do.
static int process_lmmse_cl(dt_iop_module_t *self,
                            dt_dev_pixelpipe_iop_t *piece,
                            cl_mem dev_in,
                            cl_mem dev_out,
                            const int width,
                            const int height,
                            const uint32_t filters,
                            const dt_iop_demosaic_lmmse_t mode)
{
  const dt_iop_demosaic_global_data_t *gd = self->global_data;
  const int devid = piece->pipe->devid;
  const int pw = width + 8;
  const int ph = height + 8;
  const size_t bsize = sizeof(float) * pw * ph;
  const float scaler = dt_iop_get_processed_maximum(piece);
  const float revscaler = 1.0f / scaler;

  float h0 = 1.0f;
  float h1 = expf(-1.0f / 8.0f);
  float h2 = expf(-4.0f / 8.0f);
  float h3 = expf(-9.0f / 8.0f);
  float h4 = expf(-16.0f / 8.0f);
  const float hs = h0 + 2.0f * (h1 + h2 + h3 + h4);
  h0 /= hs;
  h1 /= hs;
  h2 /= hs;
  h3 /= hs;
  h4 /= hs;

  const int medians = (mode < DT_LMMSE_REFINE_2) ? mode : 3;
  const int refine = (mode > DT_LMMSE_REFINE_2) ? mode - 2 : 0;

  enum { NBUF = 6 };
  cl_mem b[NBUF] = { NULL };
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  // the border of demosaic_ppg(..., 4) in lmmse_demosaic()
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_border_interpolate, width, height,
          CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  {
    dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = sizeof(float) * 1, .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

    err = dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt);
    if(err != CL_SUCCESS) goto error;

    const size_t sizes[2] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey) };
    const size_t local[2] = { locopt.sizex, locopt.sizey };
    err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_ppg_green, sizes, local,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(filters),
        CLLOCAL(sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3)), CLARGINT(7));
    if(err != CL_SUCCESS) goto error;
  }

  {
    dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

    err = dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt);
    if(err != CL_SUCCESS) goto error;

    const size_t sizes[2] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey) };
    const size_t local[2] = { locopt.sizex, locopt.sizey };
    err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_ppg_redblue, sizes, local,
      CLARG(dev_tmp), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(filters),
      CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)), CLARGINT(4));
    if(err != CL_SUCCESS) goto error;
  }
  dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  for(int k = 0; k < NBUF; k++)
    if(!(b[k] = dt_opencl_alloc_device_buffer(devid, bsize))) goto error;

  cl_mem hdiff = b[0], vdiff = b[1], hlp = b[2], vlp = b[3], interp = b[4], cfa = b[5];
  cl_mem red = b[0], green = b[1], blue = b[2], corr_r = b[3], corr_b = b[4];

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_init, pw, ph,
          CLARG(dev_in), CLARG(cfa), CLARG(width), CLARG(height), CLARG(pw), CLARG(ph), CLARG(revscaler));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_diff, pw, ph,
          CLARG(cfa), CLARG(hdiff), CLARG(vdiff), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_lowpass, pw, ph,
          CLARG(hdiff), CLARG(vdiff), CLARG(hlp), CLARG(vlp), CLARG(pw), CLARG(ph),
          CLARG(h0), CLARG(h1), CLARG(h2), CLARG(h3), CLARG(h4));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_interp, pw, ph,
          CLARG(hdiff), CLARG(vdiff), CLARG(hlp), CLARG(vlp), CLARG(interp), CLARG(pw), CLARG(ph),
          CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_planes, pw, ph,
          CLARG(cfa), CLARG(interp), CLARG(red), CLARG(green), CLARG(blue),
          CLARG(width), CLARG(height), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_rb_at_g, pw, ph,
          CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_rb_at_rb, pw, ph,
          CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  for(int pass = 0; pass < medians; pass++)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_median, pw, ph,
            CLARG(red), CLARG(green), CLARG(blue), CLARG(corr_r), CLARG(corr_b), CLARG(pw), CLARG(ph));
    if(err != CL_SUCCESS) goto error;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_median_apply, pw, ph,
            CLARG(red), CLARG(green), CLARG(blue), CLARG(corr_r), CLARG(corr_b), CLARG(pw), CLARG(ph),
            CLARG(filters));
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_fill, pw, ph,
          CLARG(cfa), CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  for(int step = 0; step < refine; step++)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_refine_g, pw, ph,
            CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
    if(err != CL_SUCCESS) goto error;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_refine_rb_at_g, pw, ph,
            CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
    if(err != CL_SUCCESS) goto error;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_refine_rb_at_rb, pw, ph,
            CLARG(red), CLARG(green), CLARG(blue), CLARG(pw), CLARG(ph), CLARG(filters));
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lmmse_output, width, height,
          CLARG(red), CLARG(green), CLARG(blue), CLARG(dev_out),
          CLARG(width), CLARG(height), CLARG(pw), CLARG(scaler));

error:
  dt_opencl_release_mem_object(dev_tmp);
  for(int k = 0; k < NBUF; k++) dt_opencl_release_mem_object(b[k]);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] lmmse problem '%s'", cl_errstr(err));
  return err;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
