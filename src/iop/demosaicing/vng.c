/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

/* taken from dcraw and demosaic_ppg below */

static void lin_interpolate(float *out,
                            const float *const in,
                            const int width,
                            const int height,
                            const uint32_t filters,
                            const uint8_t (*const xtrans)[6])
{
  const int colors = (filters == 9) ? 3 : 4;
  // border interpolate
  DT_OMP_FOR()
  for(int row = 0; row < height; row++)
    for(int col = 0; col < width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      uint8_t count[4] = { 0 };
      if(col == 1 && row >= 1 && row < height - 1)
        col = width - 1;
      // average all the adjoining pixels inside image by color
      for(int y = row - 1; y != row + 2; y++)
        for(int x = col - 1; x != col + 2; x++)
          if(y >= 0 && x >= 0 && y < height && x < width)
          {
            const int f = fcol(y, x, filters, xtrans);
            sum[f] += fmaxf(0.0f, in[y * width + x]);
            count[f]++;
          }
      const int f = fcol(row, col, filters, xtrans);
      // for current cell, copy the current sensor's color data,
      // interpolate the other two colors from surrounding pixels of
      // their color
      for(int c = 0; c < colors; c++)
      {
        if(c != f && count[c] != 0)
          out[4 * (row * width + col) + c] = sum[c] / count[c];
        else
          out[4 * (row * width + col) + c] = fmaxf(0.0f, in[row * width + col]);
      }
    }

  // build interpolation lookup table which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  // NUM_PIXELS                 # of neighboring pixels to read
  // for(1..NUM_PIXELS):
  //   OFFSET                   # in bytes from current pixel
  //   WEIGHT                   # how much weight to give this neighbor
  //   COLOR                    # sensor color
  // # weights of adjoining pixels not of this pixel's color
  // COLORA TOT_WEIGHT
  // COLORB TOT_WEIGHT
  // COLORPIX                   # color of center pixel

  int(*const lookup)[16][32] = malloc(sizeof(int) * 16 * 16 * 32);

  const int size = (filters == 9) ? 6 : 16;
  for(int row = 0; row < size; row++)
    for(int col = 0; col < size; col++)
    {
      int *ip = &(lookup[row][col][1]);
      int sum[4] = { 0 };
      const int f = fcol(row, col, filters, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y, col + x, filters, xtrans);
          if(color == f) continue;
          *ip++ = width * y + x;
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3; /* # of neighboring pixels found */
      for(int c = 0; c < colors; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }

  DT_OMP_FOR()
  for(int row = 1; row < height - 1; row++)
  {
    float *buf = out + 4 * width * row + 4;
    const float *buf_in = in + width * row + 1;
    for(int col = 1; col < width - 1; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      int *ip = &(lookup[row % size][col % size][0]);
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for(int i = *ip++; i--; ip += 3)
        sum[ip[2]] += fmaxf(0.0f, buf_in[ip[0]]) * ip[1];
      // for each interpolated color, load it into the pixel
      for(int i = colors; --i; ip += 2)
        buf[*ip] = sum[ip[0]] / ip[1];
      buf[*ip] = fmaxf(0.0f, *buf_in);
      buf += 4;
      buf_in++;
    }
  }

  free(lookup);
}


// VNG interpolate adapted from dcraw 9.20

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://scien.stanford.edu/pages/labsite/1999/psych221/projects/99/tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
*/
static void _copy_abovezero(float *to, float *from, const int pixels)
{
  static dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f};
  for(int i = 0; i < pixels; i++)
    dt_vector_max(&to[i*4], zero, &from[i*4]);
}

static const signed char terms[]
      = { -2, -2, +0, -1, 1, 0x01, -2, -2, +0, +0, 2, 0x01, -2, -1, -1, +0, 1, 0x01, -2, -1, +0, -1, 1, 0x02,
          -2, -1, +0, +0, 1, 0x03, -2, -1, +0, +1, 2, 0x01, -2, +0, +0, -1, 1, 0x06, -2, +0, +0, +0, 2, 0x02,
          -2, +0, +0, +1, 1, 0x03, -2, +1, -1, +0, 1, 0x04, -2, +1, +0, -1, 2, 0x04, -2, +1, +0, +0, 1, 0x06,
          -2, +1, +0, +1, 1, 0x02, -2, +2, +0, +0, 2, 0x04, -2, +2, +0, +1, 1, 0x04, -1, -2, -1, +0, 1, 0x80,
          -1, -2, +0, -1, 1, 0x01, -1, -2, +1, -1, 1, 0x01, -1, -2, +1, +0, 2, 0x01, -1, -1, -1, +1, 1, 0x88,
          -1, -1, +1, -2, 1, 0x40, -1, -1, +1, -1, 1, 0x22, -1, -1, +1, +0, 1, 0x33, -1, -1, +1, +1, 2, 0x11,
          -1, +0, -1, +2, 1, 0x08, -1, +0, +0, -1, 1, 0x44, -1, +0, +0, +1, 1, 0x11, -1, +0, +1, -2, 2, 0x40,
          -1, +0, +1, -1, 1, 0x66, -1, +0, +1, +0, 2, 0x22, -1, +0, +1, +1, 1, 0x33, -1, +0, +1, +2, 2, 0x10,
          -1, +1, +1, -1, 2, 0x44, -1, +1, +1, +0, 1, 0x66, -1, +1, +1, +1, 1, 0x22, -1, +1, +1, +2, 1, 0x10,
          -1, +2, +0, +1, 1, 0x04, -1, +2, +1, +0, 2, 0x04, -1, +2, +1, +1, 1, 0x04, +0, -2, +0, +0, 2, 0x80,
          +0, -1, +0, +1, 2, 0x88, +0, -1, +1, -2, 1, 0x40, +0, -1, +1, +0, 1, 0x11, +0, -1, +2, -2, 1, 0x40,
          +0, -1, +2, -1, 1, 0x20, +0, -1, +2, +0, 1, 0x30, +0, -1, +2, +1, 2, 0x10, +0, +0, +0, +2, 2, 0x08,
          +0, +0, +2, -2, 2, 0x40, +0, +0, +2, -1, 1, 0x60, +0, +0, +2, +0, 2, 0x20, +0, +0, +2, +1, 1, 0x30,
          +0, +0, +2, +2, 2, 0x10, +0, +1, +1, +0, 1, 0x44, +0, +1, +1, +2, 1, 0x10, +0, +1, +2, -1, 2, 0x40,
          +0, +1, +2, +0, 1, 0x60, +0, +1, +2, +1, 1, 0x20, +0, +1, +2, +2, 1, 0x10, +1, -2, +1, +0, 1, 0x80,
          +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10 };
static const signed char chood[]
    = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };

static void vng_interpolate(float *out,
                            const float *const in,
                            const int width,
                            const int height,
                            const uint32_t filters,
                            const uint8_t (*const xtrans)[6],
                            const gboolean only_vng_linear)
{
  int *ip, *code[16][16];
  // ring buffer pointing to three most recent rows processed (brow[3]
  // is only used for rotating the buffer
  float(*brow[4])[4];
  const gboolean is_xtrans = (filters == 9);
  const gboolean is_4bayer = FILTERS_ARE_4BAYER(filters);
  const gboolean is_bayer = !(is_xtrans || is_4bayer);
  const int prow = is_xtrans ? 6 : 8;
  const int pcol = is_xtrans ? 6 : 2;
  const int colors = is_xtrans ? 3 : 4;

  // separate out G1 and G2 in RGGB Bayer patterns
  uint32_t filters4 = filters;
  if(is_xtrans || is_4bayer)
    filters4 = filters;
  else if((filters & 3) == 1)
    filters4 = filters | 0x03030303u;
  else
    filters4 = filters | 0x0c0c0c0cu;

  lin_interpolate(out, in, width, height, filters4, xtrans);

  // if only linear interpolation is requested we can stop it here
  if(only_vng_linear)
  {
    if(is_bayer) goto bayer_greens;
    else return;
  }

  char *buffer = dt_alloc_aligned(sizeof(**brow) * width * 3 + sizeof(*ip) * prow * pcol * 320);
  if(!buffer)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate VNG buffer");
    return;
  }
  for(int row = 0; row < 3; row++)
    brow[row] = (float(*)[4])buffer + row * width;
  ip = (int *)(buffer + sizeof(**brow) * width * 3);

  for(int row = 0; row < prow; row++) /* Precalculate for VNG */
    for(int col = 0; col < pcol; col++)
    {
      code[row][col] = ip;
      const signed char *cp = terms;
      for(int t = 0; t < 64; t++)
      {
        const int y1 = *cp++, x1 = *cp++;
        const int y2 = *cp++, x2 = *cp++;
        const int weight = *cp++;
        const int grads = *cp++;
        const int color = fcol(row + y1, col + x1, filters4, xtrans);
        if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
        const int diag = (fcol(row, col + 1, filters4, xtrans) == color && fcol(row + 1, col, filters4, xtrans) == color)
                  ? 2
                  : 1;
        if(abs(y1 - y2) == diag && abs(x1 - x2) == diag) continue;
        *ip++ = (y1 * width + x1) * 4 + color;
        *ip++ = (y2 * width + x2) * 4 + color;
        *ip++ = weight;
        for(int g = 0; g < 8; g++)
          if(grads & 1 << g) *ip++ = g;
        *ip++ = -1;
      }
      *ip++ = INT_MAX;
      cp = chood;
      for(int g = 0; g < 8; g++)
      {
        const int y = *cp++, x = *cp++;
        *ip++ = (y * width + x) * 4;
        const int color = fcol(row, col, filters4, xtrans);
        if(fcol(row + y, col + x, filters4, xtrans) != color && fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)
          *ip++ = (y * width + x) * 8 + color;
        else
          *ip++ = 0;
      }
    }

  for(int row = 2; row < height - 2; row++) /* Do VNG interpolation */
  {
    DT_OMP_FOR(private(ip))
    for(int col = 2; col < width - 2; col++)
    {
      int g;
      float gval[8] = { 0.0f };
      float *pix = out + 4 * (row * width + col);
      ip = code[row % prow][col % pcol];
      while((g = ip[0]) != INT_MAX) /* Calculate gradients */
      {
        float diff = fabsf(pix[g] - pix[ip[1]]) * ip[2];
        gval[ip[3]] += diff;
        ip += 5;
        if((g = ip[-1]) == -1) continue;
        gval[g] += diff;
        while((g = *ip++) != -1)
          gval[g] += diff;
      }
      ip++;
      float gmin = gval[0], gmax = gval[0]; /* Choose a threshold */
      for(g = 1; g < 8; g++)
      {
        if(gmin > gval[g]) gmin = gval[g];
        if(gmax < gval[g]) gmax = gval[g];
      }
      if(gmax == 0)
      {
        memcpy(brow[2][col], pix, sizeof(*out) * 4);
        continue;
      }
      const float thold = gmin + (gmax * 0.5f);
      dt_aligned_pixel_t sum = { 0.0f };
      const int color = fcol(row, col, filters4, xtrans);
      int num = 0;
      for(g = 0; g < 8; g++, ip += 2) /* Average the neighbors */
      {
        if(gval[g] <= thold)
        {
          for(int c = 0; c < colors; c++)
            if(c == color && ip[1])
              sum[c] += (pix[c] + pix[ip[1]]) * 0.5f;
            else
              sum[c] += pix[ip[0] + c];
          num++;
        }
      }
      for(int c = 0; c < colors; c++) /* Save to buffer */
      {
        float tot = pix[color];
        if(c != color) tot += (sum[c] - sum[color]) / num;
        brow[2][col][c] = tot;
      }
    }
    if(row > 3) /* Write buffer to image */
      _copy_abovezero(out + 4 * ((row - 2) * width + 2), (float *)(brow[0] + 2), width - 4);

    // rotate ring buffer
    for(int g = 0; g < 4; g++) brow[(g - 1) & 3] = brow[g];
  }
  // copy the final two rows to the image
  _copy_abovezero(out + (4 * ((height - 4) * width + 2)), (float *)(brow[0] + 2), width - 4);
  _copy_abovezero(out + (4 * ((height - 3) * width + 2)), (float *)(brow[1] + 2), width - 4);
  dt_free_align(buffer);

bayer_greens:
  if(is_bayer)
  {
    DT_OMP_FOR()
    for(int i = 0; i < height * width; i++)
      out[i * 4 + 1] = (out[i * 4 + 1] + out[i * 4 + 3]) / 2.0f;
  }
}

#ifdef HAVE_OPENCL
static cl_int process_vng_cl(const dt_iop_module_t *self,
                             const dt_dev_pixelpipe_iop_t *piece,
                             cl_mem dev_in,
                             cl_mem dev_out,
                             cl_mem dev_xtrans,
                             const uint8_t (*const xtrans)[6],
                             const int width,
                             const int height,
                             const uint32_t filters,
                             const int border,
                             const gboolean only_vng_linear)
{
  const dt_iop_demosaic_global_data_t *gd = self->global_data;
  const gboolean is_xtrans = (filters == 9u);
 
  // separate out G1 and G2 in Bayer patterns
  uint32_t filters4;
  if(is_xtrans)
    filters4 = filters;
  else if((filters & 3) == 1)
    filters4 = filters | 0x03030303u;
  else
    filters4 = filters | 0x0c0c0c0cu;

  const int lsize = is_xtrans ? 6 : 16;
  const int colors = is_xtrans ? 3 : 4;
  const int prow = is_xtrans ? 6 : 8;
  const int pcol = is_xtrans ? 6 : 2;
  const int devid = piece->pipe->devid;

  int *ips = NULL;

  cl_mem dev_tmp = NULL;
  cl_mem dev_lookup = NULL;
  cl_mem dev_code = NULL;
  cl_mem dev_ips = NULL;
  cl_mem tmp_out = NULL;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const size_t lookup_size = (size_t)16 * 16 * 32 * sizeof(int32_t);
  int32_t(*lookup)[16][32] = malloc(lookup_size);
  if(!lookup) goto finish;
  // build interpolation lookup table for linear interpolation which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  for(int row = 0; row < lsize; row++)
  {
    for(int col = 0; col < lsize; col++)
    {
      int32_t *ip = &(lookup[row][col][1]);
      int sum[4] = { 0 };
      const int f = fcol(row, col, filters4, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
      {
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y, col + x, filters4, xtrans);
          if(color == f) continue;
          *ip++ = (y << 16) | (x & 0xffffu);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      }
      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3; /* # of neighboring pixels found */
      for(int c = 0; c < colors; c++)
      {
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      }
      *ip = f;
    }
  }

  if(!only_vng_linear)
  {
    const size_t ips_size = (size_t)prow * pcol * 352 * sizeof(int);
    ips = malloc(ips_size);

    int *ip = ips;
    int code[16][16];

    for(int row = 0; row < prow; row++)
    {
      for(int col = 0; col < pcol; col++)
      {
        code[row][col] = ip - ips;
        const signed char *cp = terms;
        for(int t = 0; t < 64; t++)
        {
          const int y1 = *cp++, x1 = *cp++;
          const int y2 = *cp++, x2 = *cp++;
          const int weight = *cp++;
          const int grads = *cp++;
          const int color = fcol(row + y1, col + x1, filters4, xtrans);
          if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
          const int diag
              = (fcol(row, col + 1, filters4, xtrans) == color && fcol(row + 1, col, filters4, xtrans) == color)
                    ? 2
                    : 1;
          if(abs(y1 - y2) == diag && abs(x1 - x2) == diag) continue;
          *ip++ = (y1 << 16) | (x1 & 0xffffu);
          *ip++ = (y2 << 16) | (x2 & 0xffffu);
          *ip++ = (color << 16) | (weight & 0xffffu);
          for(int g = 0; g < 8; g++)
            if(grads & 1 << g) *ip++ = g;
          *ip++ = -1;
        }
        *ip++ = INT_MAX;
        cp = chood;
        for(int g = 0; g < 8; g++)
        {
          const int y = *cp++, x = *cp++;
          *ip++ = (y << 16) | (x & 0xffffu);
          const int color = fcol(row, col, filters4, xtrans);
          if(fcol(row + y, col + x, filters4, xtrans) != color
             && fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)
          {
            *ip++ = (2*y << 16) | (2*x & 0xffffu);
            *ip++ = color;
          }
          else
          {
            *ip++ = 0;
            *ip++ = 0;
          }
        }
      }
    }

    dev_code = dt_opencl_copy_host_to_device_constant(devid, sizeof(code), code);
    dev_ips = dt_opencl_copy_host_to_device_constant(devid, ips_size, ips);
    if(!dev_ips || !dev_code) goto finish;
  }

  dev_lookup = dt_opencl_copy_host_to_device_constant(devid, lookup_size, lookup);

  if(only_vng_linear)
    tmp_out = dev_out;
  else
  {
    dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
    tmp_out = dev_tmp;
  }

  if(!dev_lookup || !tmp_out) goto finish;

  {
    // do linear interpolation
    dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 1 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_lin_interpolate, &locopt))
    {
      err = CL_INVALID_WORK_DIMENSION;
      goto finish;
    }
    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_vng_lin_interpolate, 0,
        CLARG(dev_in), CLARG(tmp_out),
        CLARG(width), CLARG(height), CLARG(border),
        CLARG(filters4), CLARG(dev_xtrans), CLARG(dev_lookup),
        CLLOCAL(sizeof(float) * (locopt.sizex + 2) * (locopt.sizey + 2)),
        CLARG(only_vng_linear));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_lin_interpolate, sizes, local);
    if(err != CL_SUCCESS) goto finish;
  }

  if(only_vng_linear)
    goto finish;

  // do full VNG interpolation; linear data is in dev_tmp
  dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_interpolate, &locopt))
  {
    err = CL_INVALID_WORK_DIMENSION;
    goto finish;
  }
  size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
  size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
  dt_opencl_set_kernel_args(devid, gd->kernel_vng_interpolate, 0,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_out),
        CLARG(width), CLARG(height), CLARG(filters4),
        CLARG(dev_xtrans), CLARG(dev_ips), CLARG(dev_code), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 4) * (locopt.sizey + 4)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_interpolate, sizes, local);

finish:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_lookup);
  free(lookup);
  dt_opencl_release_mem_object(dev_code);
  dt_opencl_release_mem_object(dev_ips);
  free(ips);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] vng problem '%s'", cl_errstr(err));
  return err;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

