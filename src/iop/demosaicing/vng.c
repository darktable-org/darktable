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

/* taken from dcraw and demosaic_ppg below */

static void lin_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6])
{
  const int colors = (filters == 9) ? 3 : 4;

// border interpolate
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, filters, in, roi_in, roi_out, xtrans) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 0; row < roi_out->height; row++)
    for(int col = 0; col < roi_out->width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      uint8_t count[4] = { 0 };
      if(col == 1 && row >= 1 && row < roi_out->height - 1) col = roi_out->width - 1;
      // average all the adjoining pixels inside image by color
      for(int y = row - 1; y != row + 2; y++)
        for(int x = col - 1; x != col + 2; x++)
          if(y >= 0 && x >= 0 && y < roi_in->height && x < roi_in->width)
          {
            const int f = fcol(y + roi_in->y, x + roi_in->x, filters, xtrans);
            sum[f] += in[y * roi_in->width + x];
            count[f]++;
          }
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // for current cell, copy the current sensor's color data,
      // interpolate the other two colors from surrounding pixels of
      // their color
      for(int c = 0; c < colors; c++)
      {
        if(c != f && count[c] != 0)
          out[4 * (row * roi_out->width + col) + c] = fmaxf(0.0f, sum[c] / count[c]);
        else
          out[4 * (row * roi_out->width + col) + c] = fmaxf(0.0f, in[row * roi_in->width + col]);
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
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters, xtrans);
          if(color == f) continue;
          *ip++ = (roi_in->width * y + x);
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

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, in, lookup, roi_in, roi_out, size) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 1; row < roi_out->height - 1; row++)
  {
    float *buf = out + 4 * roi_out->width * row + 4;
    const float *buf_in = in + roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width - 1; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      int *ip = &(lookup[row % size][col % size][0]);
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for(int i = *ip++; i--; ip += 3) sum[ip[2]] += buf_in[ip[0]] * ip[1];
      // for each interpolated color, load it into the pixel
      for(int i = colors; --i; ip += 2) buf[*ip] = sum[ip[0]] / ip[1];
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
static inline void _ensure_abovezero(float *to, float *from, const int floats)
{
  for(int i = 0; i < floats; i++)
    to[i] = fmaxf(0.0f, from[i]);
}

static void vng_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6],
        const gboolean only_vng_linear)
{
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
  int *ip, *code[16][16];
  // ring buffer pointing to three most recent rows processed (brow[3]
  // is only used for rotating the buffer
  float(*brow[4])[4];
  const int width = roi_out->width, height = roi_out->height;
  const int prow = (filters == 9) ? 6 : 8;
  const int pcol = (filters == 9) ? 6 : 2;
  const int colors = (filters == 9) ? 3 : 4;

  // separate out G1 and G2 in RGGB Bayer patterns
  uint32_t filters4 = filters;
  if(filters == 9 || FILTERS_ARE_4BAYER(filters)) // x-trans or CYGM/RGBE
    filters4 = filters;
  else if((filters & 3) == 1)
    filters4 = filters | 0x03030303u;
  else
    filters4 = filters | 0x0c0c0c0cu;

  lin_interpolate(out, in, roi_out, roi_in, filters4, xtrans);

  // if only linear interpolation is requested we can stop it here
  if(only_vng_linear) return;

  char *buffer
      = (char *)dt_alloc_align(64, sizeof(**brow) * width * 3 + sizeof(*ip) * prow * pcol * 320);
  if(!buffer)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate VNG buffer\n");
    return;
  }
  for(int row = 0; row < 3; row++) brow[row] = (float(*)[4])buffer + row * width;
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
        const int diag
            = (fcol(row, col + 1, filters4, xtrans) == color && fcol(row + 1, col, filters4, xtrans) == color)
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
        if(fcol(row + y, col + x, filters4, xtrans) != color
           && fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)
          *ip++ = (y * width + x) * 8 + color;
        else
          *ip++ = 0;
      }
    }

  for(int row = 2; row < height - 2; row++) /* Do VNG interpolation */
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(colors, pcol, prow, roi_in, width, xtrans) \
    shared(row, code, brow, out, filters4) \
    private(ip) \
    schedule(static)
#endif
    for(int col = 2; col < width - 2; col++)
    {
      int g;
      float gval[8] = { 0.0f };
      float *pix = out + 4 * (row * width + col);
      ip = code[(row + roi_in->y) % prow][(col + roi_in->x) % pcol];
      while((g = ip[0]) != INT_MAX) /* Calculate gradients */
      {
        float diff = fabsf(pix[g] - pix[ip[1]]) * ip[2];
        gval[ip[3]] += diff;
        ip += 5;
        if((g = ip[-1]) == -1) continue;
        gval[g] += diff;
        while((g = *ip++) != -1) gval[g] += diff;
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
      const int color = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
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
      _ensure_abovezero(out + 4 * ((row - 2) * width + 2), (float *)(brow[0] + 2), 4 * (width - 4));

    // rotate ring buffer
    for(int g = 0; g < 4; g++) brow[(g - 1) & 3] = brow[g];
  }
  // copy the final two rows to the image
  _ensure_abovezero(out + (4 * ((height - 4) * width + 2)), (float *)(brow[0] + 2), 4 * (width - 4));
  _ensure_abovezero(out + (4 * ((height - 3) * width + 2)), (float *)(brow[1] + 2), 4 * (width - 4));
  dt_free_align(buffer);

  if(filters != 9 && !FILTERS_ARE_4BAYER(filters)) // x-trans or CYGM/RGBE
// for Bayer mix the two greens to make VNG4
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(height, width) \
    shared(out) \
    schedule(static)
#endif
    for(int i = 0; i < height * width; i++) out[i * 4 + 1] = (out[i * 4 + 1] + out[i * 4 + 3]) / 2.0f;
}

#ifdef HAVE_OPENCL
static int process_vng_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean smooth,
        const gboolean only_vng_linear)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  // separate out G1 and G2 in Bayer patterns
  uint32_t filters4;
  if(piece->pipe->dsc.filters == 9u)
    filters4 = piece->pipe->dsc.filters;
  else if((piece->pipe->dsc.filters & 3) == 1)
    filters4 = piece->pipe->dsc.filters | 0x03030303u;
  else
    filters4 = piece->pipe->dsc.filters | 0x0c0c0c0cu;

  const int size = (filters4 == 9u) ? 6 : 16;
  const int colors = (filters4 == 9u) ? 3 : 4;
  const int prow = (filters4 == 9u) ? 6 : 8;
  const int pcol = (filters4 == 9u) ? 6 : 2;
  const int devid = piece->pipe->devid;

  const float processed_maximum[4]
      = { piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1],
          piece->pipe->dsc.processed_maximum[2], 1.0f };

  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  int *ips = NULL;

  cl_mem dev_tmp = NULL;
  cl_mem dev_aux = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_lookup = NULL;
  cl_mem dev_code = NULL;
  cl_mem dev_ips = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  int32_t(*lookup)[16][32] = NULL;

  if(piece->pipe->dsc.filters == 9u)
  {
    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    // build interpolation lookup table for linear interpolation which for a given offset in the sensor
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
    const size_t lookup_size = (size_t)16 * 16 * 32 * sizeof(int32_t);
    lookup = malloc(lookup_size);

    for(int row = 0; row < size; row++)
      for(int col = 0; col < size; col++)
      {
        int32_t *ip = &(lookup[row][col][1]);
        int sum[4] = { 0 };
        const int f = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
        // make list of adjoining pixel offsets by weight & color
        for(int y = -1; y <= 1; y++)
          for(int x = -1; x <= 1; x++)
          {
            const int weight = 1 << ((y == 0) + (x == 0));
            const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters4, xtrans);
            if(color == f) continue;
            *ip++ = (y << 16) | (x & 0xffffu);
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

    // Precalculate for VNG
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

    const size_t ips_size = (size_t)prow * pcol * 352 * sizeof(int);
    ips = malloc(ips_size);

    int *ip = ips;
    int code[16][16];

    for(int row = 0; row < prow; row++)
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


    dev_lookup = dt_opencl_copy_host_to_device_constant(devid, lookup_size, lookup);
    if(dev_lookup == NULL) goto error;

    dev_code = dt_opencl_copy_host_to_device_constant(devid, sizeof(code), code);
    if(dev_code == NULL) goto error;

    dev_ips = dt_opencl_copy_host_to_device_constant(devid, ips_size, ips);
    if(dev_ips == NULL) goto error;

    // green equilibration for Bayer sensors
    if(piece->pipe->dsc.filters != 9u && data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in))
        goto error;

      dev_in = dev_green_eq;
    }

    int width = roi_out->width;
    int height = roi_out->height;

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

    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(dev_tmp == NULL) goto error;

    {
      // manage borders for linear interpolation part
      const int border = 1;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(border), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(filters4), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // do linear interpolation
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 1 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_lin_interpolate, &locopt))
        goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_vng_lin_interpolate, 0, CLARG(dev_in), CLARG(dev_tmp),
        CLARG(width), CLARG(height), CLARG(filters4), CLARG(dev_lookup), CLLOCAL(sizeof(float) * (locopt.sizex + 2) * (locopt.sizey + 2)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_lin_interpolate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }


    if(qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR)
    {
      // leave it at linear interpolation and skip VNG
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_aux, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // do full VNG interpolation
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_interpolate, &locopt))
        goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_vng_interpolate, 0, CLARG(dev_tmp), CLARG(dev_aux),
        CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(filters4), CLARRAY(4, processed_maximum),
        CLARG(dev_xtrans), CLARG(dev_ips), CLARG(dev_code), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 4) * (locopt.sizey + 4)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_interpolate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    {
      // manage borders
      const int border = 2;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(border), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(filters4), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }

    if(filters4 != 9)
    {
      // for Bayer sensors mix the two green channels
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_aux, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_green_equilibrate, width, height,
        CLARG(dev_tmp), CLARG(dev_aux), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }

    if(piece->pipe->want_detail_mask)
      dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, TRUE);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample half-size or third-size image
    if(piece->pipe->dsc.filters == 9u)
    {
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, width, height,
        CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      const int zero = 0;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, width, height,
        CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dev_aux = NULL;

  dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;

  dt_opencl_release_mem_object(dev_lookup);
  dev_lookup = NULL;

  free(lookup);

  dt_opencl_release_mem_object(dev_code);
  dev_code = NULL;

  dt_opencl_release_mem_object(dev_ips);
  dev_ips = NULL;

  dt_opencl_release_mem_object(dev_green_eq);
  dev_green_eq = NULL;

  free(ips);
  ips = NULL;

  // color smoothing
  if((data->color_smoothing) && smooth)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_lookup);
  free(lookup);
  dt_opencl_release_mem_object(dev_code);
  dt_opencl_release_mem_object(dev_ips);
  dt_opencl_release_mem_object(dev_green_eq);
  free(ips);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

