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

// amaze_demosaic() on the OpenCL device, see data/kernels/demosaic_amaze.cl.
// Every CPU tile of 160 pixels, 128 apart, has its own 160x160 part of the
// buffers. The passes write zeros where they compute nothing, because later
// passes read those margins as the CPU does in its tiles; that is much
// cheaper than clearing the buffers first. Twelve buffers are live at the
// peak; later passes reuse the ones whose contents are no longer read, as
// the CPU code does with its tile buffers. The passes that update in place
// on the CPU run one 16x16 work group per tile.
static int process_amaze_cl(dt_iop_module_t *self,
                            dt_dev_pixelpipe_iop_t *piece,
                            cl_mem dev_in,
                            cl_mem dev_out,
                            const int width,
                            const int height,
                            const uint32_t filters)
{
  const dt_iop_demosaic_global_data_t *gd = self->global_data;
  const int devid = piece->pipe->devid;
  const int pw = width + 32;
  const int ph = height + 32;
  const int ntx = (pw - 16 + 127) / 128;
  const int nty = (ph - 16 + 127) / 128;
  const int gw = 160 * ntx, gh = 160 * nty;
  const size_t bsize = sizeof(float) * gw * gh;
  const float clip_pt = dt_iop_get_processed_minimum(piece);
  const float clip_pt8 = 0.8f * clip_pt;
  const size_t tiles[2] = { 16 * ntx, 16 * nty };
  const size_t local[2] = { 16, 16 };
  const int m8 = 8, m10 = 10;

  enum { NBUF = 12 };
  cl_mem b[NBUF] = { NULL };
  cl_int err = CL_SUCCESS;
  for(int k = 0; k < NBUF && err == CL_SUCCESS; k++)
  {
    b[k] = dt_opencl_alloc_device_buffer(devid, bsize);
    if(!b[k]) err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  }
  if(err != CL_SUCCESS) goto error;

  cl_mem cfa = b[0];
  // up to the hvwt pass
  cl_mem delhvsqsum = b[1], vcd = b[2], hcd = b[3], vcdalt = b[4], hcdalt = b[5];
  cl_mem dgintv = b[6], dginth = b[7], vcd2 = b[8], hcd2 = b[9];
  cl_mem hvwt = b[10], nyquist = b[11];
  // after it, in the buffers that are free by then
  cl_mem nyquist2 = b[2], hvwt2 = b[3], dgrb = b[4], green = b[5];
  cl_mem dgrb2h = b[6], dgrb2v = b[7];
  cl_mem rbm = b[8], rbp = b[1], pmwt = b[11], pmwt2 = b[10], rbint = b[2];
  cl_mem dgr = b[6], dgb = b[7];

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_init, gw, gh,
          CLARG(dev_in), CLARG(cfa), CLARG(width), CLARG(height), CLARG(pw), CLARG(ph));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_grad, gw, gh,
          CLARG(cfa), CLARG(delhvsqsum), CLARG(pw), CLARG(ph));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_cd, gw, gh,
          CLARG(cfa), CLARG(vcd), CLARG(hcd), CLARG(vcdalt), CLARG(hcdalt), CLARG(dgintv), CLARG(dginth),
          CLARG(pw), CLARG(ph), CLARG(filters), CLARG(clip_pt8));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_amaze_cd_bound, tiles, local,
          CLARG(cfa), CLARG(vcd), CLARG(hcd), CLARG(vcdalt), CLARG(hcdalt), CLARG(vcd2), CLARG(hcd2),
          CLARG(pw), CLARG(ph), CLARG(filters), CLARG(clip_pt));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_hvwt, gw, gh,
          CLARG(cfa), CLARG(vcd2), CLARG(hcd2), CLARG(dgintv), CLARG(dginth), CLARG(delhvsqsum),
          CLARG(hvwt), CLARG(nyquist), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_nyquist2, gw, gh,
          CLARG(nyquist), CLARG(nyquist2), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_area, gw, gh,
          CLARG(cfa), CLARG(nyquist2), CLARG(hvwt), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_amaze_vote, tiles, local,
          CLARG(hvwt), CLARG(hvwt2), CLARG(pw), CLARG(ph), CLARG(filters), CLARG(m8));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_green, gw, gh,
          CLARG(cfa), CLARG(vcd2), CLARG(hcd2), CLARG(hvwt2), CLARG(nyquist2), CLARG(dgrb), CLARG(green),
          CLARG(dgrb2h), CLARG(dgrb2v), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_nyquist_refine, gw, gh,
          CLARG(cfa), CLARG(vcd2), CLARG(hcd2), CLARG(nyquist2), CLARG(dgrb2h), CLARG(dgrb2v),
          CLARG(dgrb), CLARG(green), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_diag, gw, gh,
          CLARG(cfa), CLARG(rbm), CLARG(rbp), CLARG(pmwt), CLARG(pw), CLARG(ph), CLARG(filters),
          CLARG(clip_pt));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_amaze_vote, tiles, local,
          CLARG(pmwt), CLARG(pmwt2), CLARG(pw), CLARG(ph), CLARG(filters), CLARG(m10));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_rbint, gw, gh,
          CLARG(cfa), CLARG(rbm), CLARG(rbp), CLARG(pmwt2), CLARG(rbint), CLARG(pw), CLARG(ph),
          CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_green_diag, gw, gh,
          CLARG(cfa), CLARG(hvwt2), CLARG(pmwt2), CLARG(rbint), CLARG(dgrb), CLARG(green),
          CLARG(pw), CLARG(ph), CLARG(filters), CLARG(clip_pt));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_chroma, gw, gh,
          CLARG(dgrb), CLARG(dgr), CLARG(dgb), CLARG(pw), CLARG(ph), CLARG(filters));
  if(err != CL_SUCCESS) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_amaze_output, width, height,
          CLARG(cfa), CLARG(green), CLARG(hvwt2), CLARG(dgr), CLARG(dgb), CLARG(dev_out),
          CLARG(width), CLARG(height), CLARG(pw), CLARG(filters));

error:
  for(int k = 0; k < NBUF; k++) dt_opencl_release_mem_object(b[k]);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] amaze problem '%s'", cl_errstr(err));
  return err;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
