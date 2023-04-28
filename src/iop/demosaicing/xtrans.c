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

// xtrans_interpolate adapted from dcraw 9.20

// tile size, optimized to keep data in L2 cache
#define TS 122

/** Lookup for allhex[], making sure that row/col aren't negative **/
static inline const short *_hexmap(
        const int row,
        const int col,
        short (*const allhex)[3][8])
{
  // Row and column offsets may be negative, but C's modulo function
  // is not useful here with a negative dividend. To be safe, add a
  // fairly large multiple of 3. In current code row and col will
  // never be less than -9 (1-pass) or -14 (3-pass).
  int irow = row + 600;
  int icol = col + 600;
  assert(irow >= 0 && icol >= 0);
  return allhex[irow % 3][icol % 3];
}

/*
   Frank Markesteijn's algorithm for Fuji X-Trans sensors
*/
static void xtrans_markesteijn_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint8_t (*const xtrans)[6],
        const int passes)
{
  static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                     patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                     { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } },
                     dir[4] = { 1, TS, TS + 1, TS - 1 };

  short allhex[3][3][8];
  // sgrow/sgcol is the offset in the sensor matrix of the solitary
  // green pixels (initialized here only to avoid compiler warning)
  unsigned short sgrow = 0, sgcol = 0;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const unsigned ndir = 4 << (passes > 1);

  const size_t buffer_size = (size_t)TS * TS * (ndir * 4 + 3) * sizeof(float);
  size_t padded_buffer_size;
  char *const all_buffers = (char *)dt_alloc_perthread(buffer_size, sizeof(char), &padded_buffer_size);
  if(!all_buffers)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate Markesteijn buffers\n");
    return;
  }

  /* Map a green hexagon around each non-green pixel and vice versa:    */
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      for(int ng = 0, d = 0; d < 10; d += 2)
      {
        const int g = FCxtrans(row, col, NULL, xtrans) == 1;
        if(FCxtrans(row + orth[d], col + orth[d + 2], NULL, xtrans) == 1)
          ng = 0;
        else
          ng++;
        // if there are four non-green pixels adjacent in cardinal
        // directions, this is the solitary green pixel
        if(ng == 4)
        {
          sgrow = row;
          sgcol = col;
        }
        if(ng == g + 1)
          for(int c = 0; c < 8; c++)
          {
            const int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
            const int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
            // offset within TSxTS buffer
            allhex[row][col][c ^ (g * 2 & d)] = h + v * TS;
          }
      }

  // extra passes propagates out errors at edges, hence need more padding
  const int pad_tile = (passes == 1) ? 12 : 17;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(all_buffers, padded_buffer_size, dir, height, in, ndir, pad_tile, passes, roi_in, width, xtrans) \
  shared(sgrow, sgcol, allhex, out) \
  schedule(static)
#endif
  // step through TSxTS cells of image, each tile overlapping the
  // prior as interpolation needs a substantial border
  for(int top = -pad_tile; top < height - pad_tile; top += TS - (pad_tile*2))
  {
    char *const buffer = dt_get_perthread(all_buffers, padded_buffer_size);
    // rgb points to ndir TSxTS tiles of 3 channels (R, G, and B)
    float(*rgb)[TS][TS][3] = (float(*)[TS][TS][3])buffer;
    // yuv points to 3 channel (Y, u, and v) TSxTS tiles
    // note that channels come before tiles to allow for a
    // vectorization optimization when building drv[] from yuv[]
    float (*const yuv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    // drv points to ndir TSxTS tiles, each a single channel of derivatives
    float (*const drv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3 + 3) * sizeof(float));
    // gmin and gmax reuse memory which is used later by yuv buffer;
    // each points to a TSxTS tile of single channel data
    float (*const gmin)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    float (*const gmax)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3 + 1) * sizeof(float));
    // homo and homosum reuse memory which is used earlier in the
    // loop; each points to ndir single-channel TSxTS tiles
    uint8_t (*const homo)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    uint8_t (*const homosum)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float)
                                                            + TS * TS * ndir * sizeof(uint8_t));

    for(int left = -pad_tile; left < width - pad_tile; left += TS - (pad_tile*2))
    {
      int mrow = MIN(top + TS, height + pad_tile);
      int mcol = MIN(left + TS, width + pad_tile);

      // Copy current tile from in to image buffer. If border goes
      // beyond edges of image, fill with mirrored/interpolated edges.
      // The extra border avoids discontinuities at image edges.
      for(int row = top; row < mrow; row++)
        for(int col = left; col < mcol; col++)
        {
          float(*const pix) = rgb[0][row - top][col - left];
          if((col >= 0) && (row >= 0) && (col < width) && (row < height))
          {
            const int f = FCxtrans(row, col, roi_in, xtrans);
            for(int c = 0; c < 3; c++) pix[c] = (c == f) ? in[roi_in->width * row + col] : 0.f;
          }
          else
          {
            // mirror a border pixel if beyond image edge
            const int c = FCxtrans(row, col, roi_in, xtrans);
            for(int cc = 0; cc < 3; cc++)
            {
              if(cc != c)
                pix[cc] = 0.0f;
              else
              {
#define TRANSLATE(n, size) ((n >= size) ? (2 * size - n - 2) : abs(n))
                const int cy = TRANSLATE(row, height), cx = TRANSLATE(col, width);
                if(c == FCxtrans(cy, cx, roi_in, xtrans))
                  pix[c] = in[roi_in->width * cy + cx];
                else
                {
                  // interpolate if mirror pixel is a different color
                  float sum = 0.0f;
                  uint8_t count = 0;
                  for(int y = row - 1; y <= row + 1; y++)
                    for(int x = col - 1; x <= col + 1; x++)
                    {
                      const int yy = TRANSLATE(y, height), xx = TRANSLATE(x, width);
                      const int ff = FCxtrans(yy, xx, roi_in, xtrans);
                      if(ff == c)
                      {
                        sum += in[roi_in->width * yy + xx];
                        count++;
                      }
                    }
                  pix[c] = sum / count;
                }
              }
            }
          }
        }

      // duplicate rgb[0] to rgb[1], rgb[2], and rgb[3]
      for(int c = 1; c <= 3; c++) memcpy(rgb[c], rgb[0], sizeof(*rgb));

      // note that successive calculations are inset within the tile
      // so as to give enough border data, and there needs to be a 6
      // pixel border initially to allow allhex to find neighboring
      // pixels

      /* Set green1 and green3 to the minimum and maximum allowed values:   */
      // Run through each red/blue or blue/red pair, setting their g1
      // and g3 values to the min/max of green pixels surrounding the
      // pair. Use a 3 pixel border as gmin/gmax is used by
      // interpolate green which has a 3 pixel border.
      const int pad_g1_g3 = 3;
      for(int row = top + pad_g1_g3; row < mrow - pad_g1_g3; row++)
      {
        // setting max to 0.0f signifies that this is a new pair, which
        // requires a new min/max calculation of its neighboring greens
        float min = FLT_MAX, max = 0.0f;
        for(int col = left + pad_g1_g3; col < mcol - pad_g1_g3; col++)
        {
          // if in row of horizontal red & blue pairs (or processing
          // vertical red & blue pairs near image bottom), reset min/max
          // between each pair
          if(FCxtrans(row, col, roi_in, xtrans) == 1)
          {
            min = FLT_MAX, max = 0.0f;
            continue;
          }
          // if at start of red & blue pair, calculate min/max of green
          // pixels surrounding it; note that while normally using == to
          // compare floats is suspect, here the check is if 0.0f has
          // explicitly been assigned to max (which signifies a new
          // red/blue pair)
          if(max == 0.0f)
          {
            float (*const pix)[3] = &rgb[0][row - top][col - left];
            const short *const hex = _hexmap(row,col,allhex);
            for(int c = 0; c < 6; c++)
            {
              const float val = pix[hex[c]][1];
              if(min > val) min = val;
              if(max < val) max = val;
            }
          }
          gmin[row - top][col - left] = min;
          gmax[row - top][col - left] = max;
          // handle vertical red/blue pairs
          switch((row - sgrow) % 3)
          {
            // hop down a row to second pixel in vertical pair
            case 1:
              if(row < mrow - 4) row++, col--;
              break;
            // then if not done with the row hop up and right to next
            // vertical red/blue pair, resetting min/max
            case 2:
              min = FLT_MAX, max = 0.0f;
              if((col += 2) < mcol - 4 && row > top + 3) row--;
          }
        }
      }

      /* Interpolate green horizontally, vertically, and along both diagonals: */
      // need a 3 pixel border here as 3*hex[] can have a 3 unit offset
      const int pad_g_interp = 3;
      for(int row = top + pad_g_interp; row < mrow - pad_g_interp; row++)
        for(int col = left + pad_g_interp; col < mcol - pad_g_interp; col++)
        {
          float color[8];
          const int f = FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float (*const pix)[3] = &rgb[0][row - top][col - left];
          const short *const hex = _hexmap(row,col,allhex);
          // TODO: these constants come from integer math constants in
          // dcraw -- calculate them instead from interpolation math
          color[0] = 0.6796875f * (pix[hex[1]][1] + pix[hex[0]][1])
                     - 0.1796875f * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
          color[1] = 0.87109375f * pix[hex[3]][1] + pix[hex[2]][1] * 0.13f
                     + 0.359375f * (pix[0][f] - pix[-hex[2]][f]);
          for(int c = 0; c < 2; c++)
            color[2 + c] = 0.640625f * pix[hex[4 + c]][1] + 0.359375f * pix[-2 * hex[4 + c]][1]
                           + 0.12890625f * (2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
          for(int c = 0; c < 4; c++)
            rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1]
                = CLAMPS(color[c], gmin[row - top][col - left], gmax[row - top][col - left]);
        }

      for(int pass = 0; pass < passes; pass++)
      {
        if(pass == 1)
        {
          // if on second pass, copy rgb[0] to [3] into rgb[4] to [7],
          // and process that second set of buffers
          memcpy(rgb + 4, rgb, sizeof(*rgb) * 4);
          rgb += 4;
        }

        /* Recalculate green from interpolated values of closer pixels: */
        if(pass)
        {
          const int pad_g_recalc = 6;
          for(int row = top + pad_g_recalc; row < mrow - pad_g_recalc; row++)
            for(int col = left + pad_g_recalc; col < mcol - pad_g_recalc; col++)
            {
              const int f = FCxtrans(row, col, roi_in, xtrans);
              if(f == 1) continue;
              const short *const hex = _hexmap(row,col,allhex);
              for(int d = 3; d < 6; d++)
              {
                float(*rfx)[3] = &rgb[(d - 2) ^ !((row - sgrow) % 3)][row - top][col - left];
                const float val = rfx[-2 * hex[d]][1]
                            + 2 * rfx[hex[d]][1] - rfx[-2 * hex[d]][f]
                            - 2 * rfx[hex[d]][f] + 3 * rfx[0][f];
                rfx[0][1] = CLAMPS(val / 3.0f, gmin[row - top][col - left], gmax[row - top][col - left]);
              }
            }
        }

        /* Interpolate red and blue values for solitary green pixels:   */
        const int pad_rb_g = (passes == 1) ? 6 : 5;
        for(int row = (top - sgrow + pad_rb_g + 2) / 3 * 3 + sgrow; row < mrow - pad_rb_g; row += 3)
          for(int col = (left - sgcol + pad_rb_g + 2) / 3 * 3 + sgcol; col < mcol - pad_rb_g; col += 3)
          {
            float(*rfx)[3] = &rgb[0][row - top][col - left];
            int h = FCxtrans(row, col + 1, roi_in, xtrans);
            float diff[6] = { 0.0f };
            // interplated color: first index is red/blue, second is
            // pass, is double actual result
            float color[2][6];
            // Six passes, alternating hori/vert interp (i),
            // starting with R or B (h) depending on which is closest.
            // Passes 0,1 to rgb[0], rgb[1] of hori/vert interp. Pass
            // 3,5 to rgb[2], rgb[3] of best of interp hori/vert
            // results. Each pass which outputs moves on to the next
            // rgb[] for input of interp greens.
            for(int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2)
            {
              // look 1 and 2 pixels distance from solitary green to
              // red then blue or blue then red
              for(int c = 0; c < 2; c++, h ^= 2)
              {
                // rate of change in greens between current pixel and
                // interpolated pixels 1 or 2 distant: a quick
                // derivative which will be divided by two later to be
                // rate of luminance change for red/blue between known
                // red/blue neighbors and the current unknown pixel
                const float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-(i << c)][1];
                // color is halved before being stored in rgb, hence
                // this becomes green rate of change plus the average
                // of the near red or blue pixels on current axis
                color[h != 0][d] = g + rfx[i << c][h] + rfx[-(i << c)][h];
                // Note that diff will become the slope for both red
                // and blue differentials in the current direction.
                // For 2nd and 3rd hori+vert passes, create a sum of
                // steepness for both cardinal directions.
                if(d > 1)
                  diff[d] += sqrf(rfx[i << c][1] - rfx[-(i << c)][1] - rfx[i << c][h] + rfx[-(i << c)][h])
                             + sqrf(g);
              }
              if((d < 2) || (d & 1))
              { // output for passes 0, 1, 3, 5
                // for 0, 1 just use hori/vert, for 3, 5 use best of x/y dir
                const int d_out = d - ((d > 1) && (diff[d-1] < diff[d]));
                rfx[0][0] = color[0][d_out] / 2.f;
                rfx[0][2] = color[1][d_out] / 2.f;
                rfx += TS * TS;
              }
            }
          }

        /* Interpolate red for blue pixels and vice versa:              */
        const int pad_rb_br = (passes == 1) ? 6 : 5;
        for(int row = top + pad_rb_br; row < mrow - pad_rb_br; row++)
          for(int col = left + pad_rb_br; col < mcol - pad_rb_br; col++)
          {
            const int f = 2 - FCxtrans(row, col, roi_in, xtrans);
            if(f == 1) continue;
            float(*rfx)[3] = &rgb[0][row - top][col - left];
            const int c = (row - sgrow) % 3 ? TS : 1;
            const int h = 3 * (c ^ TS ^ 1);
            for(int d = 0; d < 4; d++, rfx += TS * TS)
            {
              const int i = d > 1 || ((d ^ c) & 1) ||
                ((fabsf(rfx[0][1]-rfx[c][1]) + fabsf(rfx[0][1]-rfx[-c][1])) <
                 2.f*(fabsf(rfx[0][1]-rfx[h][1]) + fabsf(rfx[0][1]-rfx[-h][1]))) ? c:h;
              rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
            }
          }

        /* Fill in red and blue for 2x2 blocks of green:                */
        const int pad_g22 = (passes == 1) ? 8 : 4;
        for(int row = top + pad_g22; row < mrow - pad_g22; row++)
        {
          if((row - sgrow) % 3)
            for(int col = left + pad_g22; col < mcol - pad_g22; col++)
              if((col - sgcol) % 3)
              {
                float(*rfx)[3] = &rgb[0][row - top][col - left];
                const short *const hex = _hexmap(row,col,allhex);
                for(unsigned d = 0; d < ndir; d += 2, rfx += TS * TS)
                  if(hex[d] + hex[d + 1])
                  {
                    const float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                  }
                  else
                  {
                    const float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
                  }
              }
        }
      } // end of multipass loop

      // jump back to the first set of rgb buffers (this is a nop
      // unless on the second pass)
      rgb = (float(*)[TS][TS][3])buffer;
      // from here on out, mainly are working within the current tile
      // rather than in reference to the image, so don't offset
      // mrow/mcol by top/left of tile
      mrow -= top;
      mcol -= left;

      /* Convert to perceptual colorspace and differentiate in all directions:  */
      // Original dcraw algorithm uses CIELab as perceptual space
      // (presumably coming from original AHD) and converts taking
      // camera matrix into account. Now use YPbPr which requires much
      // less code and is nearly indistinguishable. It assumes the
      // camera RGB is roughly linear.
      for(unsigned d = 0; d < ndir; ++d)
      {
        const int pad_yuv = (passes == 1) ? 8 : 13;
        for(int row = pad_yuv; row < mrow - pad_yuv; row++)
          for(int col = pad_yuv; col < mcol - pad_yuv; col++)
          {
            const float *rx = rgb[d][row][col];
            // use ITU-R BT.2020 YPbPr, which is great, but could use
            // a better/simpler choice? note that imageop.h provides
            // dt_iop_RGB_to_YCbCr which uses Rec. 601 conversion,
            // which appears less good with specular highlights
            const float y = 0.2627f * rx[0] + 0.6780f * rx[1] + 0.0593f * rx[2];
            yuv[0][row][col] = y;
            yuv[1][row][col] = (rx[2] - y) * 0.56433f;
            yuv[2][row][col] = (rx[0] - y) * 0.67815f;
          }
        // Note that f can offset by a column (-1 or +1) and by a row
        // (-TS or TS). The row-wise offsets cause the undefined
        // behavior sanitizer to warn of an out of bounds index, but
        // as yfx is multi-dimensional and there is sufficient
        // padding, that is not actually so.
        const int f = dir[d & 3];
        const int pad_drv = (passes == 1) ? 9 : 14;
        for(int row = pad_drv; row < mrow - pad_drv; row++)
          for(int col = pad_drv; col < mcol - pad_drv; col++)
          {
            const float(*yfx)[TS][TS] = (float(*)[TS][TS]) & yuv[0][row][col];
            drv[d][row][col] = sqrf(2 * yfx[0][0][0] - yfx[0][0][f] - yfx[0][0][-f])
                               + sqrf(2 * yfx[1][0][0] - yfx[1][0][f] - yfx[1][0][-f])
                               + sqrf(2 * yfx[2][0][0] - yfx[2][0][f] - yfx[2][0][-f]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, sizeof(uint8_t) * ndir * TS * TS);
      const int pad_homo = (passes == 1) ? 10 : 15;
      for(int row = pad_homo; row < mrow - pad_homo; row++)
        for(int col = pad_homo; col < mcol - pad_homo; col++)
        {
          float tr = FLT_MAX;
          for(unsigned d = 0; d < ndir; ++d)
            if(tr > drv[d][row][col]) tr = drv[d][row][col];
          tr *= 8;
          for(unsigned d = 0; d < ndir; ++d)
            for(int v = -1; v <= 1; v++)
              for(int h = -1; h <= 1; h++)
                homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
        }

      /* Build 5x5 sum of homogeneity maps for each pixel & direction */
      for(unsigned d = 0; d < ndir; ++d)
        for(int row = pad_tile; row < mrow - pad_tile; row++)
        {
          // start before first column where homo[d][row][col+2] != 0,
          // so can know v5sum and homosum[d][row][col] will be 0
          int col = pad_tile-5;
          uint8_t v5sum[5] = { 0 };
          homosum[d][row][col] = 0;
          // calculate by rolling through column sums
          for(col++; col < mcol - pad_tile; col++)
          {
            uint8_t colsum = 0;
            for(int v = -2; v <= 2; v++) colsum += homo[d][row + v][col + 2];
            homosum[d][row][col] = homosum[d][row][col - 1] - v5sum[col % 5] + colsum;
            v5sum[col % 5] = colsum;
          }
        }

      /* Average the most homogeneous pixels for the final result:       */
      for(int row = pad_tile; row < mrow - pad_tile; row++)
        for(int col = pad_tile; col < mcol - pad_tile; col++)
        {
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(unsigned d = 0; d < ndir; ++d)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          for(unsigned d = 0; d < ndir - 4; ++d)
          {
            if(hm[d] < hm[d + 4])
              hm[d] = 0;
            else if(hm[d] > hm[d + 4])
              hm[d + 4] = 0;
          }
          dt_aligned_pixel_t avg = { 0.0f };
          for(unsigned d = 0; d < ndir; ++d)
          {
            if(hm[d] >= maxval)
            {
              for(int c = 0; c < 3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]++;
            }
          }
          for(int c = 0; c < 3; c++)
            out[4 * (width * (row + top) + col + left) + c] = avg[c]/avg[3];
        }
    }
  }
  dt_free_align(all_buffers);
}

#undef TS

#define TS 122
static void xtrans_fdc_interpolate(
        struct dt_iop_module_t *self,
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint8_t (*const xtrans)[6])
{
  static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                     patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                     { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } },
                     dir[4] = { 1, TS, TS + 1, TS - 1 };

  static const float directionality[8] = { 1.0f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f };

  short allhex[3][3][8];
  // sgrow/sgcol is the offset in the sensor matrix of the solitary
  // green pixels (initialized here only to avoid compiler warning)
  unsigned short sgrow = 0, sgcol = 0;

  const int width = roi_out->width;
  const int height = roi_out->height;
  static const int ndir = 4;

  static const float complex Minv[3][8] = {
    { 1.000000e+00f, 2.500000e-01f - 4.330127e-01f * _Complex_I, -2.500000e-01f - 4.330127e-01f * _Complex_I,
      -1.000000e+00f, 7.500000e-01f - 1.299038e+00f * _Complex_I, -2.500000e-01f + 4.330127e-01f * _Complex_I,
      7.500000e-01f + 1.299038e+00f * _Complex_I, 2.500000e-01f + 4.330127e-01f * _Complex_I },
    { 1.000000e+00f, -2.000000e-01f + 3.464102e-01f * _Complex_I, 2.000000e-01f + 3.464102e-01f * _Complex_I,
      8.000000e-01f, 0.0f, 2.000000e-01f - 3.464102e-01f * _Complex_I, 0.0f,
      -2.000000e-01f - 3.464102e-01f * _Complex_I },
    { 1.000000e+00f, 2.500000e-01f - 4.330127e-01f * _Complex_I, -2.500000e-01f - 4.330127e-01f * _Complex_I,
      -1.000000e+00f, -7.500000e-01f + 1.299038e+00f * _Complex_I, -2.500000e-01f + 4.330127e-01f * _Complex_I,
      -7.500000e-01f - 1.299038e+00f * _Complex_I, 2.500000e-01f + 4.330127e-01f * _Complex_I },
  };

  static const float complex modarr[6][6][8] = {
    { { 1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 3.673940e-16f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        -1.000000e+00f - 3.673940e-16f * _Complex_I, -1.000000e+00f - 1.224647e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f + 2.449294e-16f * _Complex_I },
      { 1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I } },
    { { 5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 2.449294e-16f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.266216e-15f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I } },
    { { -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 1.133108e-15f * _Complex_I } },
    { { -1.000000e+00f + 1.224647e-16f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        -1.000000e+00f - 1.224647e-16f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I },
      { 1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 3.673940e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 2.449294e-16f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f + 4.898587e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { -1.000000e+00f - 3.673940e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f + 7.347881e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I } },
    { { -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 7.347881e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I } },
    { { 5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.266216e-15f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 1.133108e-15f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 7.347881e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 7.347881e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I } },
  };

  static const float complex harr[4][13][13]
      = { { { 1.326343e-03f - 1.299441e-18f * _Complex_I, 7.091837e-04f - 1.228342e-03f * _Complex_I,
              -6.278557e-04f - 1.087478e-03f * _Complex_I, -1.157216e-03f + 9.920263e-19f * _Complex_I,
              -4.887166e-04f + 8.464820e-04f * _Complex_I, 5.758687e-04f + 9.974338e-04f * _Complex_I,
              1.225183e-03f - 9.002496e-19f * _Complex_I, 5.758687e-04f - 9.974338e-04f * _Complex_I,
              -4.887166e-04f - 8.464820e-04f * _Complex_I, -1.157216e-03f + 7.085902e-19f * _Complex_I,
              -6.278557e-04f + 1.087478e-03f * _Complex_I, 7.091837e-04f + 1.228342e-03f * _Complex_I,
              1.326343e-03f - 6.497206e-19f * _Complex_I },
            { -1.980815e-03f + 1.698059e-18f * _Complex_I, -1.070384e-03f + 1.853959e-03f * _Complex_I,
              7.924697e-04f + 1.372598e-03f * _Complex_I, 1.876584e-03f - 1.378892e-18f * _Complex_I,
              1.225866e-03f - 2.123262e-03f * _Complex_I, -1.569320e-03f - 2.718142e-03f * _Complex_I,
              -3.273971e-03f + 2.004729e-18f * _Complex_I, -1.569320e-03f + 2.718142e-03f * _Complex_I,
              1.225866e-03f + 2.123262e-03f * _Complex_I, 1.876584e-03f - 9.192611e-19f * _Complex_I,
              7.924697e-04f - 1.372598e-03f * _Complex_I, -1.070384e-03f - 1.853959e-03f * _Complex_I,
              -1.980815e-03f + 7.277398e-19f * _Complex_I },
            { 1.457023e-03f - 1.070603e-18f * _Complex_I, 8.487143e-04f - 1.470016e-03f * _Complex_I,
              -6.873776e-04f - 1.190573e-03f * _Complex_I, -2.668335e-03f + 1.633884e-18f * _Complex_I,
              -2.459813e-03f + 4.260521e-03f * _Complex_I, 3.238772e-03f + 5.609717e-03f * _Complex_I,
              7.074895e-03f - 3.465699e-18f * _Complex_I, 3.238772e-03f - 5.609717e-03f * _Complex_I,
              -2.459813e-03f - 4.260521e-03f * _Complex_I, -2.668335e-03f + 9.803302e-19f * _Complex_I,
              -6.873776e-04f + 1.190573e-03f * _Complex_I, 8.487143e-04f + 1.470016e-03f * _Complex_I,
              1.457023e-03f - 3.568678e-19f * _Complex_I },
            { -1.017660e-03f + 6.231370e-19f * _Complex_I, -5.415171e-04f + 9.379351e-04f * _Complex_I,
              7.255109e-04f + 1.256622e-03f * _Complex_I, 3.699792e-03f - 1.812375e-18f * _Complex_I,
              4.090356e-03f - 7.084704e-03f * _Complex_I, -6.006283e-03f - 1.040319e-02f * _Complex_I,
              -1.391431e-02f + 5.112034e-18f * _Complex_I, -6.006283e-03f + 1.040319e-02f * _Complex_I,
              4.090356e-03f + 7.084704e-03f * _Complex_I, 3.699792e-03f - 9.061876e-19f * _Complex_I,
              7.255109e-04f - 1.256622e-03f * _Complex_I, -5.415171e-04f - 9.379351e-04f * _Complex_I,
              -1.017660e-03f + 1.246274e-19f * _Complex_I },
            { 9.198983e-04f - 4.506202e-19f * _Complex_I, 6.815900e-04f - 1.180548e-03f * _Complex_I,
              -1.287335e-03f - 2.229729e-03f * _Complex_I, -5.023856e-03f + 1.845735e-18f * _Complex_I,
              -5.499048e-03f + 9.524630e-03f * _Complex_I, 9.797672e-03f + 1.697006e-02f * _Complex_I,
              2.504795e-02f - 6.134977e-18f * _Complex_I, 9.797672e-03f - 1.697006e-02f * _Complex_I,
              -5.499048e-03f - 9.524630e-03f * _Complex_I, -5.023856e-03f + 6.152449e-19f * _Complex_I,
              -1.287335e-03f + 2.229729e-03f * _Complex_I, 6.815900e-04f + 1.180548e-03f * _Complex_I,
              9.198983e-04f + 0.000000e+00f * _Complex_I },
            { -7.972663e-04f + 2.929109e-19f * _Complex_I, -1.145605e-03f + 1.984247e-03f * _Complex_I,
              1.983334e-03f + 3.435235e-03f * _Complex_I, 6.730096e-03f - 1.648398e-18f * _Complex_I,
              6.782033e-03f - 1.174683e-02f * _Complex_I, -1.392077e-02f - 2.411147e-02f * _Complex_I,
              -3.906939e-02f + 4.784620e-18f * _Complex_I, -1.392077e-02f + 2.411147e-02f * _Complex_I,
              6.782033e-03f + 1.174683e-02f * _Complex_I, 6.730096e-03f + 0.000000e+00f * _Complex_I,
              1.983334e-03f - 3.435235e-03f * _Complex_I, -1.145605e-03f - 1.984247e-03f * _Complex_I,
              -7.972663e-04f - 9.763696e-20f * _Complex_I },
            { 8.625458e-04f - 2.112628e-19f * _Complex_I, 1.431113e-03f - 2.478760e-03f * _Complex_I,
              -2.310309e-03f - 4.001572e-03f * _Complex_I, -7.706486e-03f + 9.437723e-19f * _Complex_I,
              -7.220186e-03f + 1.250573e-02f * _Complex_I, 1.587118e-02f + 2.748969e-02f * _Complex_I,
              4.765675e-02f + 0.000000e+00f * _Complex_I, 1.587118e-02f - 2.748969e-02f * _Complex_I,
              -7.220186e-03f - 1.250573e-02f * _Complex_I, -7.706486e-03f - 9.437723e-19f * _Complex_I,
              -2.310309e-03f + 4.001572e-03f * _Complex_I, 1.431113e-03f + 2.478760e-03f * _Complex_I,
              8.625458e-04f + 2.112628e-19f * _Complex_I },
            { -7.972663e-04f + 9.763696e-20f * _Complex_I, -1.145605e-03f + 1.984247e-03f * _Complex_I,
              1.983334e-03f + 3.435235e-03f * _Complex_I, 6.730096e-03f + 0.000000e+00f * _Complex_I,
              6.782033e-03f - 1.174683e-02f * _Complex_I, -1.392077e-02f - 2.411147e-02f * _Complex_I,
              -3.906939e-02f - 4.784620e-18f * _Complex_I, -1.392077e-02f + 2.411147e-02f * _Complex_I,
              6.782033e-03f + 1.174683e-02f * _Complex_I, 6.730096e-03f + 1.648398e-18f * _Complex_I,
              1.983334e-03f - 3.435235e-03f * _Complex_I, -1.145605e-03f - 1.984247e-03f * _Complex_I,
              -7.972663e-04f - 2.929109e-19f * _Complex_I },
            { 9.198983e-04f + 0.000000e+00f * _Complex_I, 6.815900e-04f - 1.180548e-03f * _Complex_I,
              -1.287335e-03f - 2.229729e-03f * _Complex_I, -5.023856e-03f - 6.152449e-19f * _Complex_I,
              -5.499048e-03f + 9.524630e-03f * _Complex_I, 9.797672e-03f + 1.697006e-02f * _Complex_I,
              2.504795e-02f + 6.134977e-18f * _Complex_I, 9.797672e-03f - 1.697006e-02f * _Complex_I,
              -5.499048e-03f - 9.524630e-03f * _Complex_I, -5.023856e-03f - 1.845735e-18f * _Complex_I,
              -1.287335e-03f + 2.229729e-03f * _Complex_I, 6.815900e-04f + 1.180548e-03f * _Complex_I,
              9.198983e-04f + 4.506202e-19f * _Complex_I },
            { -1.017660e-03f - 1.246274e-19f * _Complex_I, -5.415171e-04f + 9.379351e-04f * _Complex_I,
              7.255109e-04f + 1.256622e-03f * _Complex_I, 3.699792e-03f + 9.061876e-19f * _Complex_I,
              4.090356e-03f - 7.084704e-03f * _Complex_I, -6.006283e-03f - 1.040319e-02f * _Complex_I,
              -1.391431e-02f - 5.112034e-18f * _Complex_I, -6.006283e-03f + 1.040319e-02f * _Complex_I,
              4.090356e-03f + 7.084704e-03f * _Complex_I, 3.699792e-03f + 1.812375e-18f * _Complex_I,
              7.255109e-04f - 1.256622e-03f * _Complex_I, -5.415171e-04f - 9.379351e-04f * _Complex_I,
              -1.017660e-03f - 6.231370e-19f * _Complex_I },
            { 1.457023e-03f + 3.568678e-19f * _Complex_I, 8.487143e-04f - 1.470016e-03f * _Complex_I,
              -6.873776e-04f - 1.190573e-03f * _Complex_I, -2.668335e-03f - 9.803302e-19f * _Complex_I,
              -2.459813e-03f + 4.260521e-03f * _Complex_I, 3.238772e-03f + 5.609717e-03f * _Complex_I,
              7.074895e-03f + 3.465699e-18f * _Complex_I, 3.238772e-03f - 5.609717e-03f * _Complex_I,
              -2.459813e-03f - 4.260521e-03f * _Complex_I, -2.668335e-03f - 1.633884e-18f * _Complex_I,
              -6.873776e-04f + 1.190573e-03f * _Complex_I, 8.487143e-04f + 1.470016e-03f * _Complex_I,
              1.457023e-03f + 1.070603e-18f * _Complex_I },
            { -1.980815e-03f - 7.277398e-19f * _Complex_I, -1.070384e-03f + 1.853959e-03f * _Complex_I,
              7.924697e-04f + 1.372598e-03f * _Complex_I, 1.876584e-03f + 9.192611e-19f * _Complex_I,
              1.225866e-03f - 2.123262e-03f * _Complex_I, -1.569320e-03f - 2.718142e-03f * _Complex_I,
              -3.273971e-03f - 2.004729e-18f * _Complex_I, -1.569320e-03f + 2.718142e-03f * _Complex_I,
              1.225866e-03f + 2.123262e-03f * _Complex_I, 1.876584e-03f + 1.378892e-18f * _Complex_I,
              7.924697e-04f - 1.372598e-03f * _Complex_I, -1.070384e-03f - 1.853959e-03f * _Complex_I,
              -1.980815e-03f - 1.698059e-18f * _Complex_I },
            { 1.326343e-03f + 6.497206e-19f * _Complex_I, 7.091837e-04f - 1.228342e-03f * _Complex_I,
              -6.278557e-04f - 1.087478e-03f * _Complex_I, -1.157216e-03f - 7.085902e-19f * _Complex_I,
              -4.887166e-04f + 8.464820e-04f * _Complex_I, 5.758687e-04f + 9.974338e-04f * _Complex_I,
              1.225183e-03f + 9.002496e-19f * _Complex_I, 5.758687e-04f - 9.974338e-04f * _Complex_I,
              -4.887166e-04f - 8.464820e-04f * _Complex_I, -1.157216e-03f - 9.920263e-19f * _Complex_I,
              -6.278557e-04f + 1.087478e-03f * _Complex_I, 7.091837e-04f + 1.228342e-03f * _Complex_I,
              1.326343e-03f + 1.299441e-18f * _Complex_I } },
          { { 9.129120e-04f - 8.943958e-19f * _Complex_I, -5.925973e-04f - 1.026409e-03f * _Complex_I,
              -5.989682e-04f + 1.037443e-03f * _Complex_I, 1.158755e-03f - 8.514393e-19f * _Complex_I,
              -8.992493e-04f - 1.557545e-03f * _Complex_I, -1.283187e-03f + 2.222546e-03f * _Complex_I,
              2.730635e-03f - 1.337625e-18f * _Complex_I, -1.283187e-03f - 2.222546e-03f * _Complex_I,
              -8.992493e-04f + 1.557545e-03f * _Complex_I, 1.158755e-03f - 2.838131e-19f * _Complex_I,
              -5.989682e-04f - 1.037443e-03f * _Complex_I, -5.925973e-04f + 1.026409e-03f * _Complex_I,
              9.129120e-04f + 0.000000e+00f * _Complex_I },
            { -5.588854e-04f - 9.680179e-04f * _Complex_I, -6.474856e-04f + 1.121478e-03f * _Complex_I,
              1.536588e-03f - 1.129066e-18f * _Complex_I, -9.123802e-04f - 1.580289e-03f * _Complex_I,
              -1.541434e-03f + 2.669842e-03f * _Complex_I, 4.379825e-03f - 9.925627e-18f * _Complex_I,
              -2.394173e-03f - 4.146830e-03f * _Complex_I, -2.189912e-03f + 3.793039e-03f * _Complex_I,
              3.082869e-03f - 3.493222e-18f * _Complex_I, -9.123802e-04f - 1.580289e-03f * _Complex_I,
              -7.682939e-04f + 1.330724e-03f * _Complex_I, 1.294971e-03f + 0.000000e+00f * _Complex_I,
              -5.588854e-04f - 9.680179e-04f * _Complex_I },
            { -5.883876e-04f + 1.019117e-03f * _Complex_I, 1.714796e-03f - 1.260012e-18f * _Complex_I,
              -1.180365e-03f - 2.044451e-03f * _Complex_I, -1.483082e-03f + 2.568774e-03f * _Complex_I,
              4.933362e-03f - 2.416651e-18f * _Complex_I, -3.296542e-03f - 5.709779e-03f * _Complex_I,
              -3.546477e-03f + 6.142678e-03f * _Complex_I, 6.593085e-03f - 1.614840e-18f * _Complex_I,
              -2.466681e-03f - 4.272417e-03f * _Complex_I, -1.483082e-03f + 2.568774e-03f * _Complex_I,
              2.360729e-03f + 0.000000e+00f * _Complex_I, -8.573982e-04f - 1.485057e-03f * _Complex_I,
              -5.883876e-04f + 1.019117e-03f * _Complex_I },
            { 1.483526e-03f - 1.090077e-18f * _Complex_I, -1.074793e-03f - 1.861596e-03f * _Complex_I,
              -1.447448e-03f + 2.507053e-03f * _Complex_I, 3.952416e-03f - 1.936126e-18f * _Complex_I,
              -3.496688e-03f - 6.056441e-03f * _Complex_I, -4.898024e-03f + 8.483627e-03f * _Complex_I,
              1.070518e-02f - 2.622012e-18f * _Complex_I, -4.898024e-03f - 8.483627e-03f * _Complex_I,
              -3.496688e-03f + 6.056441e-03f * _Complex_I, 3.952416e-03f + 0.000000e+00f * _Complex_I,
              -1.447448e-03f - 2.507053e-03f * _Complex_I, -1.074793e-03f + 1.861596e-03f * _Complex_I,
              1.483526e-03f + 3.633590e-19f * _Complex_I },
            { -9.966429e-04f - 1.726236e-03f * _Complex_I, -1.478281e-03f + 2.560458e-03f * _Complex_I,
              4.306274e-03f - 2.109466e-18f * _Complex_I, -3.294955e-03f - 5.707029e-03f * _Complex_I,
              -5.436890e-03f + 9.416970e-03f * _Complex_I, 1.556418e-02f - 3.812124e-18f * _Complex_I,
              -8.842875e-03f - 1.531631e-02f * _Complex_I, -7.782088e-03f + 1.347897e-02f * _Complex_I,
              1.087378e-02f + 0.000000e+00f * _Complex_I, -3.294955e-03f - 5.707029e-03f * _Complex_I,
              -2.153137e-03f + 3.729342e-03f * _Complex_I, 2.956562e-03f + 3.350104e-18f * _Complex_I,
              -9.966429e-04f - 1.726236e-03f * _Complex_I },
            { -1.291288e-03f + 2.236576e-03f * _Complex_I, 3.942788e-03f - 8.935208e-18f * _Complex_I,
              -2.798347e-03f - 4.846880e-03f * _Complex_I, -4.448869e-03f + 7.705666e-03f * _Complex_I,
              1.522441e-02f - 3.728906e-18f * _Complex_I, -1.175443e-02f - 2.035927e-02f * _Complex_I,
              -1.417872e-02f + 2.455826e-02f * _Complex_I, 2.350886e-02f + 0.000000e+00f * _Complex_I,
              -7.612206e-03f - 1.318473e-02f * _Complex_I, -4.448869e-03f + 7.705666e-03f * _Complex_I,
              5.596695e-03f + 1.370795e-18f * _Complex_I, -1.971394e-03f - 3.414555e-03f * _Complex_I,
              -1.291288e-03f + 2.236576e-03f * _Complex_I },
            { 2.779286e-03f - 1.361458e-18f * _Complex_I, -2.194126e-03f - 3.800338e-03f * _Complex_I,
              -3.057720e-03f + 5.296126e-03f * _Complex_I, 9.725261e-03f - 2.382002e-18f * _Complex_I,
              -8.649261e-03f - 1.498096e-02f * _Complex_I, -1.417667e-02f + 2.455472e-02f * _Complex_I,
              3.552610e-02f + 0.000000e+00f * _Complex_I, -1.417667e-02f - 2.455472e-02f * _Complex_I,
              -8.649261e-03f + 1.498096e-02f * _Complex_I, 9.725261e-03f + 2.382002e-18f * _Complex_I,
              -3.057720e-03f - 5.296126e-03f * _Complex_I, -2.194126e-03f + 3.800338e-03f * _Complex_I,
              2.779286e-03f + 1.361458e-18f * _Complex_I },
            { -1.291288e-03f - 2.236576e-03f * _Complex_I, -1.971394e-03f + 3.414555e-03f * _Complex_I,
              5.596695e-03f - 1.370795e-18f * _Complex_I, -4.448869e-03f - 7.705666e-03f * _Complex_I,
              -7.612206e-03f + 1.318473e-02f * _Complex_I, 2.350886e-02f + 0.000000e+00f * _Complex_I,
              -1.417872e-02f - 2.455826e-02f * _Complex_I, -1.175443e-02f + 2.035927e-02f * _Complex_I,
              1.522441e-02f + 3.728906e-18f * _Complex_I, -4.448869e-03f - 7.705666e-03f * _Complex_I,
              -2.798347e-03f + 4.846880e-03f * _Complex_I, 3.942788e-03f + 8.935208e-18f * _Complex_I,
              -1.291288e-03f - 2.236576e-03f * _Complex_I },
            { -9.966429e-04f + 1.726236e-03f * _Complex_I, 2.956562e-03f - 3.350104e-18f * _Complex_I,
              -2.153137e-03f - 3.729342e-03f * _Complex_I, -3.294955e-03f + 5.707029e-03f * _Complex_I,
              1.087378e-02f + 0.000000e+00f * _Complex_I, -7.782088e-03f - 1.347897e-02f * _Complex_I,
              -8.842875e-03f + 1.531631e-02f * _Complex_I, 1.556418e-02f + 3.812124e-18f * _Complex_I,
              -5.436890e-03f - 9.416970e-03f * _Complex_I, -3.294955e-03f + 5.707029e-03f * _Complex_I,
              4.306274e-03f + 2.109466e-18f * _Complex_I, -1.478281e-03f - 2.560458e-03f * _Complex_I,
              -9.966429e-04f + 1.726236e-03f * _Complex_I },
            { 1.483526e-03f - 3.633590e-19f * _Complex_I, -1.074793e-03f - 1.861596e-03f * _Complex_I,
              -1.447448e-03f + 2.507053e-03f * _Complex_I, 3.952416e-03f + 0.000000e+00f * _Complex_I,
              -3.496688e-03f - 6.056441e-03f * _Complex_I, -4.898024e-03f + 8.483627e-03f * _Complex_I,
              1.070518e-02f + 2.622012e-18f * _Complex_I, -4.898024e-03f - 8.483627e-03f * _Complex_I,
              -3.496688e-03f + 6.056441e-03f * _Complex_I, 3.952416e-03f + 1.936126e-18f * _Complex_I,
              -1.447448e-03f - 2.507053e-03f * _Complex_I, -1.074793e-03f + 1.861596e-03f * _Complex_I,
              1.483526e-03f + 1.090077e-18f * _Complex_I },
            { -5.883876e-04f - 1.019117e-03f * _Complex_I, -8.573982e-04f + 1.485057e-03f * _Complex_I,
              2.360729e-03f + 0.000000e+00f * _Complex_I, -1.483082e-03f - 2.568774e-03f * _Complex_I,
              -2.466681e-03f + 4.272417e-03f * _Complex_I, 6.593085e-03f + 1.614840e-18f * _Complex_I,
              -3.546477e-03f - 6.142678e-03f * _Complex_I, -3.296542e-03f + 5.709779e-03f * _Complex_I,
              4.933362e-03f + 2.416651e-18f * _Complex_I, -1.483082e-03f - 2.568774e-03f * _Complex_I,
              -1.180365e-03f + 2.044451e-03f * _Complex_I, 1.714796e-03f + 1.260012e-18f * _Complex_I,
              -5.883876e-04f - 1.019117e-03f * _Complex_I },
            { -5.588854e-04f + 9.680179e-04f * _Complex_I, 1.294971e-03f + 0.000000e+00f * _Complex_I,
              -7.682939e-04f - 1.330724e-03f * _Complex_I, -9.123802e-04f + 1.580289e-03f * _Complex_I,
              3.082869e-03f + 3.493222e-18f * _Complex_I, -2.189912e-03f - 3.793039e-03f * _Complex_I,
              -2.394173e-03f + 4.146830e-03f * _Complex_I, 4.379825e-03f + 9.925627e-18f * _Complex_I,
              -1.541434e-03f - 2.669842e-03f * _Complex_I, -9.123802e-04f + 1.580289e-03f * _Complex_I,
              1.536588e-03f + 1.129066e-18f * _Complex_I, -6.474856e-04f - 1.121478e-03f * _Complex_I,
              -5.588854e-04f + 9.680179e-04f * _Complex_I },
            { 9.129120e-04f + 0.000000e+00f * _Complex_I, -5.925973e-04f - 1.026409e-03f * _Complex_I,
              -5.989682e-04f + 1.037443e-03f * _Complex_I, 1.158755e-03f + 2.838131e-19f * _Complex_I,
              -8.992493e-04f - 1.557545e-03f * _Complex_I, -1.283187e-03f + 2.222546e-03f * _Complex_I,
              2.730635e-03f + 1.337625e-18f * _Complex_I, -1.283187e-03f - 2.222546e-03f * _Complex_I,
              -8.992493e-04f + 1.557545e-03f * _Complex_I, 1.158755e-03f + 8.514393e-19f * _Complex_I,
              -5.989682e-04f - 1.037443e-03f * _Complex_I, -5.925973e-04f + 1.026409e-03f * _Complex_I,
              9.129120e-04f + 8.943958e-19f * _Complex_I } },
          { { 8.228091e-04f + 0.000000e+00f * _Complex_I, -5.365069e-04f + 9.292572e-04f * _Complex_I,
              -6.011501e-04f - 1.041223e-03f * _Complex_I, 1.249890e-03f - 3.061346e-19f * _Complex_I,
              -7.632708e-04f + 1.322024e-03f * _Complex_I, -9.846035e-04f - 1.705383e-03f * _Complex_I,
              2.080486e-03f - 1.019144e-18f * _Complex_I, -9.846035e-04f + 1.705383e-03f * _Complex_I,
              -7.632708e-04f - 1.322024e-03f * _Complex_I, 1.249890e-03f - 9.184039e-19f * _Complex_I,
              -6.011501e-04f + 1.041223e-03f * _Complex_I, -5.365069e-04f - 9.292572e-04f * _Complex_I,
              8.228091e-04f - 8.061204e-19f * _Complex_I },
            { -5.616336e-04f - 9.727779e-04f * _Complex_I, 1.382894e-03f + 0.000000e+00f * _Complex_I,
              -8.694311e-04f + 1.505899e-03f * _Complex_I, -9.721139e-04f - 1.683751e-03f * _Complex_I,
              2.446785e-03f - 2.772471e-18f * _Complex_I, -1.605471e-03f + 2.780758e-03f * _Complex_I,
              -1.832781e-03f - 3.174469e-03f * _Complex_I, 3.210942e-03f - 7.276687e-18f * _Complex_I,
              -1.223392e-03f + 2.118978e-03f * _Complex_I, -9.721139e-04f - 1.683751e-03f * _Complex_I,
              1.738862e-03f - 1.277695e-18f * _Complex_I, -6.914471e-04f + 1.197621e-03f * _Complex_I,
              -5.616336e-04f - 9.727779e-04f * _Complex_I },
            { -5.723872e-04f + 9.914038e-04f * _Complex_I, -8.302721e-04f - 1.438073e-03f * _Complex_I,
              2.445280e-03f + 0.000000e+00f * _Complex_I, -1.378399e-03f + 2.387458e-03f * _Complex_I,
              -1.882898e-03f - 3.261274e-03f * _Complex_I, 4.921549e-03f - 1.205432e-18f * _Complex_I,
              -2.760152e-03f + 4.780723e-03f * _Complex_I, -2.460774e-03f - 4.262186e-03f * _Complex_I,
              3.765795e-03f - 1.844708e-18f * _Complex_I, -1.378399e-03f + 2.387458e-03f * _Complex_I,
              -1.222640e-03f - 2.117675e-03f * _Complex_I, 1.660544e-03f - 1.220148e-18f * _Complex_I,
              -5.723872e-04f + 9.914038e-04f * _Complex_I },
            { 1.226482e-03f + 3.004015e-19f * _Complex_I, -9.600816e-04f + 1.662910e-03f * _Complex_I,
              -1.495900e-03f - 2.590974e-03f * _Complex_I, 3.833507e-03f + 0.000000e+00f * _Complex_I,
              -3.167257e-03f + 5.485850e-03f * _Complex_I, -4.303595e-03f - 7.454046e-03f * _Complex_I,
              9.412791e-03f - 2.305469e-18f * _Complex_I, -4.303595e-03f + 7.454046e-03f * _Complex_I,
              -3.167257e-03f - 5.485850e-03f * _Complex_I, 3.833507e-03f - 1.877877e-18f * _Complex_I,
              -1.495900e-03f + 2.590974e-03f * _Complex_I, -9.600816e-04f - 1.662910e-03f * _Complex_I,
              1.226482e-03f - 9.012046e-19f * _Complex_I },
            { -9.898007e-04f - 1.714385e-03f * _Complex_I, 3.215120e-03f + 3.643077e-18f * _Complex_I,
              -2.507621e-03f + 4.343327e-03f * _Complex_I, -3.557798e-03f - 6.162286e-03f * _Complex_I,
              1.105198e-02f + 0.000000e+00f * _Complex_I, -7.691179e-03f + 1.332151e-02f * _Complex_I,
              -8.705793e-03f - 1.507888e-02f * _Complex_I, 1.538236e-02f - 3.767591e-18f * _Complex_I,
              -5.525988e-03f + 9.571292e-03f * _Complex_I, -3.557798e-03f - 6.162286e-03f * _Complex_I,
              5.015242e-03f - 2.456760e-18f * _Complex_I, -1.607560e-03f + 2.784375e-03f * _Complex_I,
              -9.898007e-04f - 1.714385e-03f * _Complex_I },
            { -1.414655e-03f + 2.450254e-03f * _Complex_I, -2.341263e-03f - 4.055186e-03f * _Complex_I,
              6.915775e-03f + 1.693876e-18f * _Complex_I, -5.086403e-03f + 8.809908e-03f * _Complex_I,
              -8.062191e-03f - 1.396412e-02f * _Complex_I, 2.415333e-02f + 0.000000e+00f * _Complex_I,
              -1.451128e-02f + 2.513428e-02f * _Complex_I, -1.207667e-02f - 2.091740e-02f * _Complex_I,
              1.612438e-02f - 3.949335e-18f * _Complex_I, -5.086403e-03f + 8.809908e-03f * _Complex_I,
              -3.457887e-03f - 5.989237e-03f * _Complex_I, 4.682526e-03f - 1.061161e-17f * _Complex_I,
              -1.414655e-03f + 2.450254e-03f * _Complex_I },
            { 3.039574e-03f + 1.488962e-18f * _Complex_I, -2.598226e-03f + 4.500260e-03f * _Complex_I,
              -3.750909e-03f - 6.496765e-03f * _Complex_I, 1.119776e-02f + 2.742661e-18f * _Complex_I,
              -9.210579e-03f + 1.595319e-02f * _Complex_I, -1.464762e-02f - 2.537042e-02f * _Complex_I,
              3.672076e-02f + 0.000000e+00f * _Complex_I, -1.464762e-02f + 2.537042e-02f * _Complex_I,
              -9.210579e-03f - 1.595319e-02f * _Complex_I, 1.119776e-02f - 2.742661e-18f * _Complex_I,
              -3.750909e-03f + 6.496765e-03f * _Complex_I, -2.598226e-03f - 4.500260e-03f * _Complex_I,
              3.039574e-03f - 1.488962e-18f * _Complex_I },
            { -1.414655e-03f - 2.450254e-03f * _Complex_I, 4.682526e-03f + 1.061161e-17f * _Complex_I,
              -3.457887e-03f + 5.989237e-03f * _Complex_I, -5.086403e-03f - 8.809908e-03f * _Complex_I,
              1.612438e-02f + 3.949335e-18f * _Complex_I, -1.207667e-02f + 2.091740e-02f * _Complex_I,
              -1.451128e-02f - 2.513428e-02f * _Complex_I, 2.415333e-02f + 0.000000e+00f * _Complex_I,
              -8.062191e-03f + 1.396412e-02f * _Complex_I, -5.086403e-03f - 8.809908e-03f * _Complex_I,
              6.915775e-03f - 1.693876e-18f * _Complex_I, -2.341263e-03f + 4.055186e-03f * _Complex_I,
              -1.414655e-03f - 2.450254e-03f * _Complex_I },
            { -9.898007e-04f + 1.714385e-03f * _Complex_I, -1.607560e-03f - 2.784375e-03f * _Complex_I,
              5.015242e-03f + 2.456760e-18f * _Complex_I, -3.557798e-03f + 6.162286e-03f * _Complex_I,
              -5.525988e-03f - 9.571292e-03f * _Complex_I, 1.538236e-02f + 3.767591e-18f * _Complex_I,
              -8.705793e-03f + 1.507888e-02f * _Complex_I, -7.691179e-03f - 1.332151e-02f * _Complex_I,
              1.105198e-02f + 0.000000e+00f * _Complex_I, -3.557798e-03f + 6.162286e-03f * _Complex_I,
              -2.507621e-03f - 4.343327e-03f * _Complex_I, 3.215120e-03f - 3.643077e-18f * _Complex_I,
              -9.898007e-04f + 1.714385e-03f * _Complex_I },
            { 1.226482e-03f + 9.012046e-19f * _Complex_I, -9.600816e-04f + 1.662910e-03f * _Complex_I,
              -1.495900e-03f - 2.590974e-03f * _Complex_I, 3.833507e-03f + 1.877877e-18f * _Complex_I,
              -3.167257e-03f + 5.485850e-03f * _Complex_I, -4.303595e-03f - 7.454046e-03f * _Complex_I,
              9.412791e-03f + 2.305469e-18f * _Complex_I, -4.303595e-03f + 7.454046e-03f * _Complex_I,
              -3.167257e-03f - 5.485850e-03f * _Complex_I, 3.833507e-03f + 0.000000e+00f * _Complex_I,
              -1.495900e-03f + 2.590974e-03f * _Complex_I, -9.600816e-04f - 1.662910e-03f * _Complex_I,
              1.226482e-03f - 3.004015e-19f * _Complex_I },
            { -5.723872e-04f - 9.914038e-04f * _Complex_I, 1.660544e-03f + 1.220148e-18f * _Complex_I,
              -1.222640e-03f + 2.117675e-03f * _Complex_I, -1.378399e-03f - 2.387458e-03f * _Complex_I,
              3.765795e-03f + 1.844708e-18f * _Complex_I, -2.460774e-03f + 4.262186e-03f * _Complex_I,
              -2.760152e-03f - 4.780723e-03f * _Complex_I, 4.921549e-03f + 1.205432e-18f * _Complex_I,
              -1.882898e-03f + 3.261274e-03f * _Complex_I, -1.378399e-03f - 2.387458e-03f * _Complex_I,
              2.445280e-03f + 0.000000e+00f * _Complex_I, -8.302721e-04f + 1.438073e-03f * _Complex_I,
              -5.723872e-04f - 9.914038e-04f * _Complex_I },
            { -5.616336e-04f + 9.727779e-04f * _Complex_I, -6.914471e-04f - 1.197621e-03f * _Complex_I,
              1.738862e-03f + 1.277695e-18f * _Complex_I, -9.721139e-04f + 1.683751e-03f * _Complex_I,
              -1.223392e-03f - 2.118978e-03f * _Complex_I, 3.210942e-03f + 7.276687e-18f * _Complex_I,
              -1.832781e-03f + 3.174469e-03f * _Complex_I, -1.605471e-03f - 2.780758e-03f * _Complex_I,
              2.446785e-03f + 2.772471e-18f * _Complex_I, -9.721139e-04f + 1.683751e-03f * _Complex_I,
              -8.694311e-04f - 1.505899e-03f * _Complex_I, 1.382894e-03f + 0.000000e+00f * _Complex_I,
              -5.616336e-04f + 9.727779e-04f * _Complex_I },
            { 8.228091e-04f + 8.061204e-19f * _Complex_I, -5.365069e-04f + 9.292572e-04f * _Complex_I,
              -6.011501e-04f - 1.041223e-03f * _Complex_I, 1.249890e-03f + 9.184039e-19f * _Complex_I,
              -7.632708e-04f + 1.322024e-03f * _Complex_I, -9.846035e-04f - 1.705383e-03f * _Complex_I,
              2.080486e-03f + 1.019144e-18f * _Complex_I, -9.846035e-04f + 1.705383e-03f * _Complex_I,
              -7.632708e-04f - 1.322024e-03f * _Complex_I, 1.249890e-03f + 3.061346e-19f * _Complex_I,
              -6.011501e-04f + 1.041223e-03f * _Complex_I, -5.365069e-04f - 9.292572e-04f * _Complex_I,
              8.228091e-04f + 0.000000e+00f * _Complex_I } },
          { { 1.221201e-03f + 5.982162e-19f * _Complex_I, -1.773498e-03f - 6.515727e-19f * _Complex_I,
              1.246697e-03f + 3.053526e-19f * _Complex_I, -8.215306e-04f - 1.006085e-19f * _Complex_I,
              7.609372e-04f + 0.000000e+00f * _Complex_I, -4.863927e-04f + 5.956592e-20f * _Complex_I,
              4.882100e-04f - 1.195770e-19f * _Complex_I, -4.863927e-04f + 1.786978e-19f * _Complex_I,
              7.609372e-04f - 3.727517e-19f * _Complex_I, -8.215306e-04f + 5.030424e-19f * _Complex_I,
              1.246697e-03f - 9.160579e-19f * _Complex_I, -1.773498e-03f + 1.520336e-18f * _Complex_I,
              1.221201e-03f - 1.196432e-18f * _Complex_I },
            { 7.406884e-04f - 1.282910e-03f * _Complex_I, -1.025411e-03f + 1.776065e-03f * _Complex_I,
              7.186273e-04f - 1.244699e-03f * _Complex_I, -4.025606e-04f + 6.972554e-04f * _Complex_I,
              5.908383e-04f - 1.023362e-03f * _Complex_I, -1.125190e-03f + 1.948886e-03f * _Complex_I,
              1.432695e-03f - 2.481501e-03f * _Complex_I, -1.125190e-03f + 1.948886e-03f * _Complex_I,
              5.908383e-04f - 1.023362e-03f * _Complex_I, -4.025606e-04f + 6.972554e-04f * _Complex_I,
              7.186273e-04f - 1.244699e-03f * _Complex_I, -1.025411e-03f + 1.776065e-03f * _Complex_I,
              7.406884e-04f - 1.282910e-03f * _Complex_I },
            { -7.162255e-04f - 1.240539e-03f * _Complex_I, 8.961176e-04f + 1.552121e-03f * _Complex_I,
              -6.705589e-04f - 1.161442e-03f * _Complex_I, 6.187140e-04f + 1.071644e-03f * _Complex_I,
              -1.165433e-03f - 2.018589e-03f * _Complex_I, 1.948120e-03f + 3.374242e-03f * _Complex_I,
              -2.297663e-03f - 3.979669e-03f * _Complex_I, 1.948120e-03f + 3.374242e-03f * _Complex_I,
              -1.165433e-03f - 2.018589e-03f * _Complex_I, 6.187140e-04f + 1.071644e-03f * _Complex_I,
              -6.705589e-04f - 1.161442e-03f * _Complex_I, 8.961176e-04f + 1.552121e-03f * _Complex_I,
              -7.162255e-04f - 1.240539e-03f * _Complex_I },
            { -1.280260e-03f - 7.839331e-19f * _Complex_I, 1.987108e-03f + 9.734024e-19f * _Complex_I,
              -2.614019e-03f - 9.603749e-19f * _Complex_I, 3.635167e-03f + 8.903590e-19f * _Complex_I,
              -4.954867e-03f - 6.067962e-19f * _Complex_I, 6.653220e-03f + 0.000000e+00f * _Complex_I,
              -7.600546e-03f + 9.307984e-19f * _Complex_I, 6.653220e-03f - 1.629569e-18f * _Complex_I,
              -4.954867e-03f + 1.820389e-18f * _Complex_I, 3.635167e-03f - 1.780718e-18f * _Complex_I,
              -2.614019e-03f + 1.600625e-18f * _Complex_I, 1.987108e-03f - 1.460104e-18f * _Complex_I,
              -1.280260e-03f + 1.097506e-18f * _Complex_I },
            { -5.756945e-04f + 9.971322e-04f * _Complex_I, 1.268614e-03f - 2.197304e-03f * _Complex_I,
              -2.421407e-03f + 4.194000e-03f * _Complex_I, 4.045715e-03f - 7.007384e-03f * _Complex_I,
              -5.527367e-03f + 9.573681e-03f * _Complex_I, 6.837207e-03f - 1.184239e-02f * _Complex_I,
              -7.288212e-03f + 1.262355e-02f * _Complex_I, 6.837207e-03f - 1.184239e-02f * _Complex_I,
              -5.527367e-03f + 9.573681e-03f * _Complex_I, 4.045715e-03f - 7.007384e-03f * _Complex_I,
              -2.421407e-03f + 4.194000e-03f * _Complex_I, 1.268614e-03f - 2.197304e-03f * _Complex_I,
              -5.756945e-04f + 9.971322e-04f * _Complex_I },
            { 7.349896e-04f + 1.273039e-03f * _Complex_I, -1.748057e-03f - 3.027723e-03f * _Complex_I,
              3.332671e-03f + 5.772355e-03f * _Complex_I, -6.051736e-03f - 1.048191e-02f * _Complex_I,
              9.842376e-03f + 1.704749e-02f * _Complex_I, -1.401169e-02f - 2.426897e-02f * _Complex_I,
              1.598601e-02f + 2.768858e-02f * _Complex_I, -1.401169e-02f - 2.426897e-02f * _Complex_I,
              9.842376e-03f + 1.704749e-02f * _Complex_I, -6.051736e-03f - 1.048191e-02f * _Complex_I,
              3.332671e-03f + 5.772355e-03f * _Complex_I, -1.748057e-03f - 3.027723e-03f * _Complex_I,
              7.349896e-04f + 1.273039e-03f * _Complex_I },
            { 1.400383e-03f + 1.028985e-18f * _Complex_I, -3.545886e-03f - 2.171229e-18f * _Complex_I,
              7.289370e-03f + 3.570761e-18f * _Complex_I, -1.418908e-02f - 5.212982e-18f * _Complex_I,
              2.520839e-02f + 6.174275e-18f * _Complex_I, -3.934772e-02f - 4.818706e-18f * _Complex_I,
              4.797481e-02f + 0.000000e+00f * _Complex_I, -3.934772e-02f + 4.818706e-18f * _Complex_I,
              2.520839e-02f - 6.174275e-18f * _Complex_I, -1.418908e-02f + 5.212982e-18f * _Complex_I,
              7.289370e-03f - 3.570761e-18f * _Complex_I, -3.545886e-03f + 2.171229e-18f * _Complex_I,
              1.400383e-03f - 1.028985e-18f * _Complex_I },
            { 7.349896e-04f - 1.273039e-03f * _Complex_I, -1.748057e-03f + 3.027723e-03f * _Complex_I,
              3.332671e-03f - 5.772355e-03f * _Complex_I, -6.051736e-03f + 1.048191e-02f * _Complex_I,
              9.842376e-03f - 1.704749e-02f * _Complex_I, -1.401169e-02f + 2.426897e-02f * _Complex_I,
              1.598601e-02f - 2.768858e-02f * _Complex_I, -1.401169e-02f + 2.426897e-02f * _Complex_I,
              9.842376e-03f - 1.704749e-02f * _Complex_I, -6.051736e-03f + 1.048191e-02f * _Complex_I,
              3.332671e-03f - 5.772355e-03f * _Complex_I, -1.748057e-03f + 3.027723e-03f * _Complex_I,
              7.349896e-04f - 1.273039e-03f * _Complex_I },
            { -5.756945e-04f - 9.971322e-04f * _Complex_I, 1.268614e-03f + 2.197304e-03f * _Complex_I,
              -2.421407e-03f - 4.194000e-03f * _Complex_I, 4.045715e-03f + 7.007384e-03f * _Complex_I,
              -5.527367e-03f - 9.573681e-03f * _Complex_I, 6.837207e-03f + 1.184239e-02f * _Complex_I,
              -7.288212e-03f - 1.262355e-02f * _Complex_I, 6.837207e-03f + 1.184239e-02f * _Complex_I,
              -5.527367e-03f - 9.573681e-03f * _Complex_I, 4.045715e-03f + 7.007384e-03f * _Complex_I,
              -2.421407e-03f - 4.194000e-03f * _Complex_I, 1.268614e-03f + 2.197304e-03f * _Complex_I,
              -5.756945e-04f - 9.971322e-04f * _Complex_I },
            { -1.280260e-03f - 1.097506e-18f * _Complex_I, 1.987108e-03f + 1.460104e-18f * _Complex_I,
              -2.614019e-03f - 1.600625e-18f * _Complex_I, 3.635167e-03f + 1.780718e-18f * _Complex_I,
              -4.954867e-03f - 1.820389e-18f * _Complex_I, 6.653220e-03f + 1.629569e-18f * _Complex_I,
              -7.600546e-03f - 9.307984e-19f * _Complex_I, 6.653220e-03f + 0.000000e+00f * _Complex_I,
              -4.954867e-03f + 6.067962e-19f * _Complex_I, 3.635167e-03f - 8.903590e-19f * _Complex_I,
              -2.614019e-03f + 9.603749e-19f * _Complex_I, 1.987108e-03f - 9.734024e-19f * _Complex_I,
              -1.280260e-03f + 7.839331e-19f * _Complex_I },
            { -7.162255e-04f + 1.240539e-03f * _Complex_I, 8.961176e-04f - 1.552121e-03f * _Complex_I,
              -6.705589e-04f + 1.161442e-03f * _Complex_I, 6.187140e-04f - 1.071644e-03f * _Complex_I,
              -1.165433e-03f + 2.018589e-03f * _Complex_I, 1.948120e-03f - 3.374242e-03f * _Complex_I,
              -2.297663e-03f + 3.979669e-03f * _Complex_I, 1.948120e-03f - 3.374242e-03f * _Complex_I,
              -1.165433e-03f + 2.018589e-03f * _Complex_I, 6.187140e-04f - 1.071644e-03f * _Complex_I,
              -6.705589e-04f + 1.161442e-03f * _Complex_I, 8.961176e-04f - 1.552121e-03f * _Complex_I,
              -7.162255e-04f + 1.240539e-03f * _Complex_I },
            { 7.406884e-04f + 1.282910e-03f * _Complex_I, -1.025411e-03f - 1.776065e-03f * _Complex_I,
              7.186273e-04f + 1.244699e-03f * _Complex_I, -4.025606e-04f - 6.972554e-04f * _Complex_I,
              5.908383e-04f + 1.023362e-03f * _Complex_I, -1.125190e-03f - 1.948886e-03f * _Complex_I,
              1.432695e-03f + 2.481501e-03f * _Complex_I, -1.125190e-03f - 1.948886e-03f * _Complex_I,
              5.908383e-04f + 1.023362e-03f * _Complex_I, -4.025606e-04f - 6.972554e-04f * _Complex_I,
              7.186273e-04f + 1.244699e-03f * _Complex_I, -1.025411e-03f - 1.776065e-03f * _Complex_I,
              7.406884e-04f + 1.282910e-03f * _Complex_I },
            { 1.221201e-03f + 1.196432e-18f * _Complex_I, -1.773498e-03f - 1.520336e-18f * _Complex_I,
              1.246697e-03f + 9.160579e-19f * _Complex_I, -8.215306e-04f - 5.030424e-19f * _Complex_I,
              7.609372e-04f + 3.727517e-19f * _Complex_I, -4.863927e-04f - 1.786978e-19f * _Complex_I,
              4.882100e-04f + 1.195770e-19f * _Complex_I, -4.863927e-04f - 5.956592e-20f * _Complex_I,
              7.609372e-04f + 0.000000e+00f * _Complex_I, -8.215306e-04f + 1.006085e-19f * _Complex_I,
              1.246697e-03f - 3.053526e-19f * _Complex_I, -1.773498e-03f + 6.515727e-19f * _Complex_I,
              1.221201e-03f - 5.982162e-19f * _Complex_I } } };

  const size_t buffer_size = (size_t)TS * TS * (ndir * 4 + 7) * sizeof(float);
  size_t padded_buffer_size;
  char *const all_buffers = (char *)dt_alloc_perthread(buffer_size, sizeof(char), &padded_buffer_size);
  if(!all_buffers)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate FDC base buffers\n");
    return;
  }

  /* Map a green hexagon around each non-green pixel and vice versa:    */
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      for(int ng = 0, d = 0; d < 10; d += 2)
      {
        int g = FCxtrans(row, col, NULL, xtrans) == 1;
        if(FCxtrans(row + orth[d], col + orth[d + 2], NULL, xtrans) == 1)
          ng = 0;
        else
          ng++;
        // if there are four non-green pixels adjacent in cardinal
        // directions, this is the solitary green pixel
        if(ng == 4)
        {
          sgrow = row;
          sgcol = col;
        }
        if(ng == g + 1)
          for(int c = 0; c < 8; c++)
          {
            int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
            int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
            // offset within TSxTS buffer
            allhex[row][col][c ^ (g * 2 & d)] = h + v * TS;
          }
      }

  // extra passes propagates out errors at edges, hence need more padding
  const int pad_tile = 13;

  // calculate offsets for this roi
  int rowoffset = 0;
  int coloffset = 0;
  for(int row = 0; row < 6; row++)
  {
    if(!((row - sgrow) % 3))
    {
      for(int col = 0; col < 6; col++)
      {
        if(!((col - sgcol) % 3) && (FCxtrans(row, col + 1, roi_in, xtrans) == 0))
        {
          rowoffset = 37 - row - pad_tile; // 1 plus a generous multiple of 6
          coloffset = 37 - col - pad_tile; // to avoid that this value gets negative
          break;
        }
      }
      break;
    }
  }

  // depending on the iso, use either a hybrid approach for chroma, or pure fdc
  float hybrid_fdc[2] = { 1.0f, 0.0f };
  const int xover_iso = dt_conf_get_int("plugins/darkroom/demosaic/fdc_xover_iso");
  int iso = self->dev->image_storage.exif_iso;
  if(iso > xover_iso)
  {
    hybrid_fdc[0] = 0.0f;
    hybrid_fdc[1] = 1.0f;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(ndir, all_buffers, dir, directionality, harr, height, in, Minv, modarr, roi_in, width,    \
                        xtrans, pad_tile, padded_buffer_size)                                                     \
        shared(sgrow, sgcol, allhex, out, rowoffset, coloffset, hybrid_fdc) schedule(static)
#endif
  // step through TSxTS cells of image, each tile overlapping the
  // prior as interpolation needs a substantial border
  for(int top = -pad_tile; top < height - pad_tile; top += TS - (pad_tile * 2))
  {
    char *const buffer = dt_get_perthread(all_buffers, padded_buffer_size);
    // rgb points to ndir TSxTS tiles of 3 channels (R, G, and B)
    float(*rgb)[TS][TS][3] = (float(*)[TS][TS][3])buffer;
    // yuv points to 3 channel (Y, u, and v) TSxTS tiles
    // note that channels come before tiles to allow for a
    // vectorization optimization when building drv[] from yuv[]
    float (*const yuv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    // drv points to ndir TSxTS tiles, each a single channel of derivatives
    float (*const drv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3 + 3) * sizeof(float));
    // gmin and gmax reuse memory which is used later by yuv buffer;
    // each points to a TSxTS tile of single channel data
    float (*const gmin)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    float (*const gmax)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3 + 1) * sizeof(float));
    // homo and homosum reuse memory which is used earlier in the
    // loop; each points to ndir single-channel TSxTS tiles
    uint8_t (*const homo)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    uint8_t (*const homosum)[TS][TS]
        = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float) + TS * TS * ndir * sizeof(uint8_t));
    // append all fdc related buffers
    float complex *fdc_buf_start = (float complex *)(buffer + TS * TS * (ndir * 4 + 3) * sizeof(float));
    const int fdc_buf_size = TS * TS;
    float(*const i_src) = (float *)fdc_buf_start;
    float complex(*const o_src) = fdc_buf_start + fdc_buf_size;
    // by the time the chroma values are calculated, o_src can be overwritten.
    float(*const fdc_chroma) = (float *)o_src;

    for(int left = -pad_tile; left < width - pad_tile; left += TS - (pad_tile * 2))
    {
      int mrow = MIN(top + TS, height + pad_tile);
      int mcol = MIN(left + TS, width + pad_tile);

      // Copy current tile from in to image buffer. If border goes
      // beyond edges of image, fill with mirrored/interpolated edges.
      // The extra border avoids discontinuities at image edges.
      for(int row = top; row < mrow; row++)
        for(int col = left; col < mcol; col++)
        {
          float(*const pix) = rgb[0][row - top][col - left];
          if((col >= 0) && (row >= 0) && (col < width) && (row < height))
          {
            const int f = FCxtrans(row, col, roi_in, xtrans);
            for(int c = 0; c < 3; c++) pix[c] = (c == f) ? in[roi_in->width * row + col] : 0.f;
            *(i_src + TS * (row - top) + (col - left)) = in[roi_in->width * row + col];
          }
          else
          {
            // mirror a border pixel if beyond image edge
            const int c = FCxtrans(row, col, roi_in, xtrans);
            for(int cc = 0; cc < 3; cc++)
              if(cc != c)
                pix[cc] = 0.0f;
              else
              {
#define TRANSLATE(n, size) ((n >= size) ? (2 * size - n - 2) : abs(n))
                const int cy = TRANSLATE(row, height), cx = TRANSLATE(col, width);
                if(c == FCxtrans(cy, cx, roi_in, xtrans))
                {
                  pix[c] = in[roi_in->width * cy + cx];
                  *(i_src + TS * (row - top) + (col - left)) = in[roi_in->width * cy + cx];
                }
                else
                {
                  // interpolate if mirror pixel is a different color
                  float sum = 0.0f;
                  uint8_t count = 0;
                  for(int y = row - 1; y <= row + 1; y++)
                    for(int x = col - 1; x <= col + 1; x++)
                    {
                      const int yy = TRANSLATE(y, height), xx = TRANSLATE(x, width);
                      const int ff = FCxtrans(yy, xx, roi_in, xtrans);
                      if(ff == c)
                      {
                        sum += in[roi_in->width * yy + xx];
                        count++;
                      }
                    }
                  pix[c] = sum / count;
                  *(i_src + TS * (row - top) + (col - left)) = pix[c];
                }
              }
          }
        }

      // duplicate rgb[0] to rgb[1], rgb[2], and rgb[3]
      for(int c = 1; c <= 3; c++) memcpy(rgb[c], rgb[0], sizeof(*rgb));

      // note that successive calculations are inset within the tile
      // so as to give enough border data, and there needs to be a 6
      // pixel border initially to allow allhex to find neighboring
      // pixels

      /* Set green1 and green3 to the minimum and maximum allowed values:   */
      // Run through each red/blue or blue/red pair, setting their g1
      // and g3 values to the min/max of green pixels surrounding the
      // pair. Use a 3 pixel border as gmin/gmax is used by
      // interpolate green which has a 3 pixel border.
      const int pad_g1_g3 = 3;
      for(int row = top + pad_g1_g3; row < mrow - pad_g1_g3; row++)
      {
        // setting max to 0.0f signifies that this is a new pair, which
        // requires a new min/max calculation of its neighboring greens
        float min = FLT_MAX, max = 0.0f;
        for(int col = left + pad_g1_g3; col < mcol - pad_g1_g3; col++)
        {
          // if in row of horizontal red & blue pairs (or processing
          // vertical red & blue pairs near image bottom), reset min/max
          // between each pair
          if(FCxtrans(row, col, roi_in, xtrans) == 1)
          {
            min = FLT_MAX, max = 0.0f;
            continue;
          }
          // if at start of red & blue pair, calculate min/max of green
          // pixels surrounding it; note that while normally using == to
          // compare floats is suspect, here the check is if 0.0f has
          // explicitly been assigned to max (which signifies a new
          // red/blue pair)
          if(max == 0.0f)
          {
            float (*const pix)[3] = &rgb[0][row - top][col - left];
            const short *const hex = _hexmap(row, col, allhex);
            for(int c = 0; c < 6; c++)
            {
              const float val = pix[hex[c]][1];
              if(min > val) min = val;
              if(max < val) max = val;
            }
          }
          gmin[row - top][col - left] = min;
          gmax[row - top][col - left] = max;
          // handle vertical red/blue pairs
          switch((row - sgrow) % 3)
          {
            // hop down a row to second pixel in vertical pair
            case 1:
              if(row < mrow - 4) row++, col--;
              break;
            // then if not done with the row hop up and right to next
            // vertical red/blue pair, resetting min/max
            case 2:
              min = FLT_MAX, max = 0.0f;
              if((col += 2) < mcol - 4 && row > top + 3) row--;
          }
        }
      }

      /* Interpolate green horizontally, vertically, and along both diagonals: */
      // need a 3 pixel border here as 3*hex[] can have a 3 unit offset
      const int pad_g_interp = 3;
      for(int row = top + pad_g_interp; row < mrow - pad_g_interp; row++)
        for(int col = left + pad_g_interp; col < mcol - pad_g_interp; col++)
        {
          float color[8];
          int f = FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float (*const pix)[3] = &rgb[0][row - top][col - left];
          const short *const hex = _hexmap(row, col, allhex);
          // TODO: these constants come from integer math constants in
          // dcraw -- calculate them instead from interpolation math
          color[0] = 0.6796875f * (pix[hex[1]][1] + pix[hex[0]][1])
                     - 0.1796875f * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
          color[1] = 0.87109375f * pix[hex[3]][1] + pix[hex[2]][1] * 0.13f
                     + 0.359375f * (pix[0][f] - pix[-hex[2]][f]);
          for(int c = 0; c < 2; c++)
            color[2 + c] = 0.640625f * pix[hex[4 + c]][1] + 0.359375f * pix[-2 * hex[4 + c]][1]
                           + 0.12890625f * (2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
          for(int c = 0; c < 4; c++)
            rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1]
                = CLAMPS(color[c], gmin[row - top][col - left], gmax[row - top][col - left]);
        }

      /* Interpolate red and blue values for solitary green pixels:   */
      const int pad_rb_g = 6;
      for(int row = (top - sgrow + pad_rb_g + 2) / 3 * 3 + sgrow; row < mrow - pad_rb_g; row += 3)
        for(int col = (left - sgcol + pad_rb_g + 2) / 3 * 3 + sgcol; col < mcol - pad_rb_g; col += 3)
        {
          float(*rfx)[3] = &rgb[0][row - top][col - left];
          int h = FCxtrans(row, col + 1, roi_in, xtrans);
          float diff[6] = { 0.0f };
          float color[3][8];
          for(int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2)
          {
            for(int c = 0; c < 2; c++, h ^= 2)
            {
              float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-(i << c)][1];
              color[h][d] = g + rfx[i << c][h] + rfx[-(i << c)][h];
              if(d > 1)
                diff[d] += sqrf(rfx[i << c][1] - rfx[-(i << c)][1] - rfx[i << c][h] + rfx[-(i << c)][h]) + sqrf(g);
            }
            if(d > 1 && (d & 1))
              if(diff[d - 1] < diff[d])
                for(int c = 0; c < 2; c++) color[c * 2][d] = color[c * 2][d - 1];
            if(d < 2 || (d & 1))
            {
              for(int c = 0; c < 2; c++) rfx[0][c * 2] = color[c * 2][d] / 2.f;
              rfx += TS * TS;
            }
          }
        }

      /* Interpolate red for blue pixels and vice versa:              */
      const int pad_rb_br = 6;
      for(int row = top + pad_rb_br; row < mrow - pad_rb_br; row++)
        for(int col = left + pad_rb_br; col < mcol - pad_rb_br; col++)
        {
          int f = 2 - FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float(*rfx)[3] = &rgb[0][row - top][col - left];
          int c = (row - sgrow) % 3 ? TS : 1;
          int h = 3 * (c ^ TS ^ 1);
          for(int d = 0; d < 4; d++, rfx += TS * TS)
          {
            int i = d > 1 || ((d ^ c) & 1)
                            || ((fabsf(rfx[0][1] - rfx[c][1]) + fabsf(rfx[0][1] - rfx[-c][1]))
                                < 2.f * (fabsf(rfx[0][1] - rfx[h][1]) + fabsf(rfx[0][1] - rfx[-h][1]))) ? c : h;
            rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
          }
        }

      /* Fill in red and blue for 2x2 blocks of green:                */
      const int pad_g22 = 8;
      for(int row = top + pad_g22; row < mrow - pad_g22; row++)
        if((row - sgrow) % 3)
          for(int col = left + pad_g22; col < mcol - pad_g22; col++)
            if((col - sgcol) % 3)
            {
              float redblue[3][3];
              float(*rfx)[3] = &rgb[0][row - top][col - left];
              const short *const hex = _hexmap(row, col, allhex);
              for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                if(hex[d] + hex[d + 1])
                {
                  float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                  for(int c = 0; c < 4; c += 2)
                  {
                    rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                    redblue[d][c] = rfx[0][c];
                  }
                }
                else
                {
                  float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                  for(int c = 0; c < 4; c += 2)
                  {
                    rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
                    redblue[d][c] = rfx[0][c];
                  }
                }
              // to fill in red and blue also for diagonal directions
              for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                for(int c = 0; c < 4; c += 2) rfx[0][c] = (redblue[0][c] + redblue[2][c]) * 0.5f;
           }

      // jump back to the first set of rgb buffers (this is a nop
      // unless on the second pass)
      rgb = (float(*)[TS][TS][3])buffer;
      // from here on out, mainly are working within the current tile
      // rather than in reference to the image, so don't offset
      // mrow/mcol by top/left of tile
      mrow -= top;
      mcol -= left;

      /* Convert to perceptual colorspace and differentiate in all directions:  */
      // Original dcraw algorithm uses CIELab as perceptual space
      // (presumably coming from original AHD) and converts taking
      // camera matrix into account. Now use YPbPr which requires much
      // less code and is nearly indistinguishable. It assumes the
      // camera RGB is roughly linear.
      for(int d = 0; d < ndir; d++)
      {
        const int pad_yuv = 8;
        for(int row = pad_yuv; row < mrow - pad_yuv; row++)
          for(int col = pad_yuv; col < mcol - pad_yuv; col++)
          {
            float *rx = rgb[d][row][col];
            // use ITU-R BT.2020 YPbPr, which is great, but could use
            // a better/simpler choice? note that imageop.h provides
            // dt_iop_RGB_to_YCbCr which uses Rec. 601 conversion,
            // which appears less good with specular highlights
            float y = 0.2627f * rx[0] + 0.6780f * rx[1] + 0.0593f * rx[2];
            yuv[0][row][col] = y;
            yuv[1][row][col] = (rx[2] - y) * 0.56433f;
            yuv[2][row][col] = (rx[0] - y) * 0.67815f;
          }
        // Note that f can offset by a column (-1 or +1) and by a row
        // (-TS or TS). The row-wise offsets cause the undefined
        // behavior sanitizer to warn of an out of bounds index, but
        // as yfx is multi-dimensional and there is sufficient
        // padding, that is not actually so.
        const int f = dir[d & 3];
        const int pad_drv = 9;
        for(int row = pad_drv; row < mrow - pad_drv; row++)
          for(int col = pad_drv; col < mcol - pad_drv; col++)
          {
            float(*yfx)[TS][TS] = (float(*)[TS][TS]) & yuv[0][row][col];
            drv[d][row][col] = sqrf(2 * yfx[0][0][0] - yfx[0][0][f] - yfx[0][0][-f])
                               + sqrf(2 * yfx[1][0][0] - yfx[1][0][f] - yfx[1][0][-f])
                               + sqrf(2 * yfx[2][0][0] - yfx[2][0][f] - yfx[2][0][-f]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, sizeof(uint8_t) * ndir * TS * TS);
      const int pad_homo = 10;
      for(int row = pad_homo; row < mrow - pad_homo; row++)
        for(int col = pad_homo; col < mcol - pad_homo; col++)
        {
          float tr = FLT_MAX;
          for(int d = 0; d < ndir; d++)
            if(tr > drv[d][row][col]) tr = drv[d][row][col];
          tr *= 8;
          for(int d = 0; d < ndir; d++)
            for(int v = -1; v <= 1; v++)
              for(int h = -1; h <= 1; h++) homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
        }

      /* Build 5x5 sum of homogeneity maps for each pixel & direction */
      for(int d = 0; d < ndir; d++)
        for(int row = pad_tile; row < mrow - pad_tile; row++)
        {
          // start before first column where homo[d][row][col+2] != 0,
          // so can know v5sum and homosum[d][row][col] will be 0
          int col = pad_tile - 5;
          uint8_t v5sum[5] = { 0 };
          homosum[d][row][col] = 0;
          // calculate by rolling through column sums
          for(col++; col < mcol - pad_tile; col++)
          {
            uint8_t colsum = 0;
            for(int v = -2; v <= 2; v++) colsum += homo[d][row + v][col + 2];
            homosum[d][row][col] = homosum[d][row][col - 1] - v5sum[col % 5] + colsum;
            v5sum[col % 5] = colsum;
          }
        }

      /* Calculate chroma values in fdc:       */
      const int pad_fdc = 6;
      for(int row = pad_fdc; row < mrow - pad_fdc; row++)
        for(int col = pad_fdc; col < mcol - pad_fdc; col++)
        {
          int myrow, mycol;
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(int d = 0; d < ndir; d++)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          float dircount = 0;
          float dirsum = 0.f;
          for(int d = 0; d < ndir; d++)
            if(hm[d] >= maxval)
            {
              dircount++;
              dirsum += directionality[d];
            }
          float w = dirsum / (float)dircount;
          int fdc_row, fdc_col;
          float complex C2m, C5m, C7m, C10m;
#define CONV_FILT(VAR, FILT)                                                                                      \
  VAR = 0.0f + 0.0f * _Complex_I;                                                                                 \
  for(fdc_row = 0, myrow = row - 6; fdc_row < 13; fdc_row++, myrow++)                                             \
    for(fdc_col = 0, mycol = col - 6; fdc_col < 13; fdc_col++, mycol++)                                           \
      VAR += FILT[12 - fdc_row][12 - fdc_col] * *(i_src + TS * myrow + mycol);
          CONV_FILT(C2m, harr[0])
          CONV_FILT(C5m, harr[1])
          CONV_FILT(C7m, harr[2])
          CONV_FILT(C10m, harr[3])
#undef CONV_FILT
          // build the q vector components
          myrow = (row + rowoffset) % 6;
          mycol = (col + coloffset) % 6;
          float complex modulator[8];
          for(int c = 0; c < 8; c++) modulator[c] = modarr[myrow][mycol][c];
          float complex qmat[8];
          qmat[4] = w * C10m * modulator[0] - (1.0f - w) * C2m * modulator[1];
          qmat[6] = conjf(qmat[4]);
          qmat[1] = C5m * modulator[6];
          qmat[2] = conjf(-0.5f * qmat[1]);
          qmat[5] = conjf(qmat[2]);
          qmat[3] = C7m * modulator[7];
          qmat[7] = conjf(qmat[1]);
          // get L
          C2m = qmat[4] * (conjf(modulator[0]) - conjf(modulator[1]));
          float complex C3m = qmat[6] * (modulator[2] - modulator[3]);
          float complex C6m = qmat[2] * (conjf(modulator[4]) + conjf(modulator[5]));
          float complex C12m = qmat[5] * (modulator[4] + modulator[5]);
          float complex C18m = qmat[7] * modulator[6];
          qmat[0] = *(i_src + row * TS + col) - C2m - C3m - C5m - C6m - 2.0f * C7m - C12m - C18m;
          // get the rgb components from fdc
          dt_aligned_pixel_t rgbpix = { 0.f, 0.f, 0.f };
          // multiply with the inverse matrix of M
          for(int color = 0; color < 3; color++)
            for(int c = 0; c < 8; c++)
            {
              rgbpix[color] += Minv[color][c] * qmat[c];
            }
          // now separate luma and chroma for
          // frequency domain chroma
          // and store it in fdc_chroma
          float uv[2];
          float y = 0.2627f * rgbpix[0] + 0.6780f * rgbpix[1] + 0.0593f * rgbpix[2];
          uv[0] = (rgbpix[2] - y) * 0.56433f;
          uv[1] = (rgbpix[0] - y) * 0.67815f;
          for(int c = 0; c < 2; c++) *(fdc_chroma + c * TS * TS + row * TS + col) = uv[c];
        }

      /* Average the most homogeneous pixels for the final result:       */
      for(int row = pad_tile; row < mrow - pad_tile; row++)
        for(int col = pad_tile; col < mcol - pad_tile; col++)
        {
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(int d = 0; d < ndir; d++)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          for(int d = 0; d < ndir - 4; d++)
          {
            if(hm[d] < hm[d + 4])
              hm[d] = 0;
            else if(hm[d] > hm[d + 4])
              hm[d + 4] = 0;
          }

          dt_aligned_pixel_t avg = { 0.f };
          for(int d = 0; d < ndir; d++)
          {
            if(hm[d] >= maxval)
            {
              for(int c = 0; c < 3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]++;
            }
          }
          dt_aligned_pixel_t rgbpix;
          for(int c = 0; c < 3; c++) rgbpix[c] = avg[c] / avg[3];
          // preserve all components of Markesteijn for this pixel
          const float y = 0.2627f * rgbpix[0] + 0.6780f * rgbpix[1] + 0.0593f * rgbpix[2];
          const float um = (rgbpix[2] - y) * 0.56433f;
          const float vm = (rgbpix[0] - y) * 0.67815f;
          float uvf[2];
          // macros for fast meadian filtering
#define PIX_SWAP(a, b)                                                                                            \
  {                                                                                                               \
    tempf = (a);                                                                                                  \
    (a) = (b);                                                                                                    \
    (b) = tempf;                                                                                                  \
  }
#define PIX_SORT(a, b)                                                                                            \
  {                                                                                                               \
    if((a) > (b)) PIX_SWAP((a), (b));                                                                             \
  }
          // instead of merely reading the values, perform 5 pixel median filter
          // one median filter is required to avoid textile artifacts
          for(int chrm = 0; chrm < 2; chrm++)
          {
            float temp[5];
            float tempf;
            // load the window into temp
            memcpy(&temp[0], fdc_chroma + chrm * TS * TS + (row - 1) * TS + (col), sizeof(float) * 1);
            memcpy(&temp[1], fdc_chroma + chrm * TS * TS + (row)*TS + (col - 1),   sizeof(float) * 3);
            memcpy(&temp[4], fdc_chroma + chrm * TS * TS + (row + 1) * TS + (col), sizeof(float) * 1);
            PIX_SORT(temp[0], temp[1]);
            PIX_SORT(temp[3], temp[4]);
            PIX_SORT(temp[0], temp[3]);
            PIX_SORT(temp[1], temp[4]);
            PIX_SORT(temp[1], temp[2]);
            PIX_SORT(temp[2], temp[3]);
            PIX_SORT(temp[1], temp[2]);
            uvf[chrm] = temp[2];
          }
          // use hybrid or pure fdc, depending on what was set above.
          // in case of hybrid, use the chroma that has the smallest
          // absolute value
          float uv[2];
          uv[0] = (((ABS(uvf[0]) < ABS(um)) & (ABS(uvf[1]) < (1.02f * ABS(vm)))) ? uvf[0] : um) * hybrid_fdc[0] + uvf[0] * hybrid_fdc[1];
          uv[1] = (((ABS(uvf[1]) < ABS(vm)) & (ABS(uvf[0]) < (1.02f * ABS(vm)))) ? uvf[1] : vm) * hybrid_fdc[0] + uvf[1] * hybrid_fdc[1];
          // combine the luma from Markesteijn with the chroma from above
          rgbpix[0] = y + 1.474600014746f * uv[1];
          rgbpix[1] = y - 0.15498578286403f * uv[0] - 0.571353132557189f * uv[1];
          rgbpix[2] = y + 1.77201282937288f * uv[0];
          for(int c = 0; c < 3; c++) out[4 * (width * (row + top) + col + left) + c] = rgbpix[c];
        }
    }
  }
  dt_free_align(all_buffers);
}

#undef PIX_SWAP
#undef PIX_SORT
#undef CCLIP
#undef TS

#ifdef HAVE_OPENCL
static int process_markesteijn_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean smooth)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  const float processed_maximum[4]
      = { piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1],
          piece->pipe->dsc.processed_maximum[2], 1.0f };

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);

  cl_mem dev_tmp = NULL;
  cl_mem dev_tmptmp = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_green_eq = NULL;
  cl_mem dev_rgbv[8] = { NULL };
  cl_mem dev_drv[8] = { NULL };
  cl_mem dev_homo[8] = { NULL };
  cl_mem dev_homosum[8] = { NULL };
  cl_mem dev_gminmax = NULL;
  cl_mem dev_allhex = NULL;
  cl_mem dev_aux = NULL;
  cl_mem dev_edge_in = NULL;
  cl_mem dev_edge_out = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem *dev_rgb = dev_rgbv;

  dev_xtrans
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
  if(dev_xtrans == NULL) goto error;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_in->width;
    int height = roi_in->height;
    const int passes = ((data->demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 3 : 1;
    const int ndir = 4 << (passes > 1);
    const int pad_tile = (passes == 1) ? 12 : 17;

    static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                       patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                       { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } };

    // allhex contains the offset coordinates (x,y) of a green hexagon around each
    // non-green pixel and vice versa
    char allhex[3][3][8][2];
    // sgreen is the offset in the sensor matrix of the solitary
    // green pixels (initialized here only to avoid compiler warning)
    char sgreen[2] = { 0 };

    // Map a green hexagon around each non-green pixel and vice versa:
    for(int row = 0; row < 3; row++)
      for(int col = 0; col < 3; col++)
        for(int ng = 0, d = 0; d < 10; d += 2)
        {
          const int g = FCxtrans(row, col, NULL, xtrans) == 1;
          if(FCxtrans(row + orth[d] + 6, col + orth[d + 2] + 6, NULL, xtrans) == 1)
            ng = 0;
          else
            ng++;
          // if there are four non-green pixels adjacent in cardinal
          // directions, this is the solitary green pixel
          if(ng == 4)
          {
            sgreen[0] = col;
            sgreen[1] = row;
          }
          if(ng == g + 1)
            for(int c = 0; c < 8; c++)
            {
              const int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
              const int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];

              allhex[row][col][c ^ (g * 2 & d)][0] = h;
              allhex[row][col][c ^ (g * 2 & d)][1] = v;
            }
        }

    dev_allhex = dt_opencl_copy_host_to_device_constant(devid, sizeof(allhex), allhex);
    if(dev_allhex == NULL) goto error;

    for(int n = 0; n < ndir; n++)
    {
      dev_rgbv[n] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
      if(dev_rgbv[n] == NULL) goto error;
    }

    dev_gminmax = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * width * height);
    if(dev_gminmax == NULL) goto error;

    dev_aux = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
    if(dev_aux == NULL) goto error;

    if(scaled)
    {
      // need to scale to right res
      dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
      if(dev_tmp == NULL) goto error;
    }
    else
    {
      // scaling factor 1.0 --> we can directly process into the output buffer
      dev_tmp = dev_out;
    }

    {
      // copy from dev_in to first rgb image buffer.
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_initial_copy, width, height,
        CLARG(dev_in), CLARG(dev_rgb[0]), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }


    // duplicate dev_rgb[0] to dev_rgb[1], dev_rgb[2], and dev_rgb[3]
    for(int c = 1; c <= 3; c++)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[0], dev_rgb[c], 0, 0,
                                                    sizeof(float) * 4 * width * height);
      if(err != CL_SUCCESS) goto error;
    }

    // find minimum and maximum allowed green values of red/blue pixel pairs
    const int pad_g1_g3 = 3;
    dt_opencl_local_buffer_t locopt_g1_g3
      = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_green_minmax, &locopt_g1_g3))
      goto error;

    {
      size_t sizes[3] = { ROUNDUP(width, locopt_g1_g3.sizex), ROUNDUP(height, locopt_g1_g3.sizey), 1 };
      size_t local[3] = { locopt_g1_g3.sizex, locopt_g1_g3.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_green_minmax, 0, CLARG(dev_rgb[0]), CLARG(dev_gminmax),
        CLARG(width), CLARG(height), CLARG(pad_g1_g3), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen),
        CLARG(dev_xtrans), CLARG(dev_allhex), CLLOCAL(sizeof(float) * (locopt_g1_g3.sizex + 2*3) * (locopt_g1_g3.sizey + 2*3)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_green_minmax, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // interpolate green horizontally, vertically, and along both diagonals
    const int pad_g_interp = 3;
    dt_opencl_local_buffer_t locopt_g_interp
      = (dt_opencl_local_buffer_t){ .xoffset = 2*6, .xfactor = 1, .yoffset = 2*6, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_interpolate_green, &locopt_g_interp))
      goto error;

    {
      size_t sizes[3] = { ROUNDUP(width, locopt_g_interp.sizex), ROUNDUP(height, locopt_g_interp.sizey), 1 };
      size_t local[3] = { locopt_g_interp.sizex, locopt_g_interp.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_interpolate_green, 0, CLARG(dev_rgb[0]),
        CLARG(dev_rgb[1]), CLARG(dev_rgb[2]), CLARG(dev_rgb[3]), CLARG(dev_gminmax), CLARG(width), CLARG(height),
        CLARG(pad_g_interp), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen), CLARG(dev_xtrans),
        CLARG(dev_allhex), CLLOCAL(sizeof(float) * 4 * (locopt_g_interp.sizex + 2*6) * (locopt_g_interp.sizey + 2*6)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_green, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // multi-pass loop: one pass for Markesteijn-1 and three passes for Markesteijn-3
    for(int pass = 0; pass < passes; pass++)
    {

      // if on second pass, copy rgb[0] to [3] into rgb[4] to [7] ....
      if(pass == 1)
      {
        for(int c = 0; c < 4; c++)
        {
          err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[c], dev_rgb[c + 4], 0, 0,
                                                        sizeof(float) * 4 * width * height);
          if(err != CL_SUCCESS) goto error;
        }
        // ... and process that second set of buffers
        dev_rgb += 4;
      }

      // second and third pass (only Markesteijn-3)
      if(pass)
      {
        // recalculate green from interpolated values of closer pixels
        const int pad_g_recalc = 6;
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_recalculate_green, width, height,
          CLARG(dev_rgb[0]), CLARG(dev_rgb[1]), CLARG(dev_rgb[2]), CLARG(dev_rgb[3]), CLARG(dev_gminmax),
          CLARG(width), CLARG(height), CLARG(pad_g_recalc), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLARG(dev_allhex));
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue values for solitary green pixels
      const int pad_rb_g = (passes == 1) ? 6 : 5;
      dt_opencl_local_buffer_t locopt_rb_g
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_solitary_green, &locopt_rb_g))
      goto error;

      cl_mem *dev_trgb = dev_rgb;
      for(int d = 0, i = 1, h = 0; d < 6; d++, i ^= 1, h ^= 2)
      {
        const char dir[2] = { i, i ^ 1 };

        // we use dev_aux to transport intermediate results from one loop run to the next
        size_t sizes[3] = { ROUNDUP(width, locopt_rb_g.sizex), ROUNDUP(height, locopt_rb_g.sizey), 1 };
        size_t local[3] = { locopt_rb_g.sizex, locopt_rb_g.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_solitary_green, 0, CLARG(dev_trgb[0]),
          CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_rb_g), CLARG(roi_in->x), CLARG(roi_in->y),
          CLARG(d), CLARRAY(2, dir), CLARG(h), CLARRAY(2, sgreen), CLARG(dev_xtrans), CLLOCAL(sizeof(float) * 4 * (locopt_rb_g.sizex + 2*2) * (locopt_rb_g.sizey + 2*2)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_solitary_green, sizes, local);
        if(err != CL_SUCCESS) goto error;

        if((d < 2) || (d & 1)) dev_trgb++;
      }

      // interpolate red for blue pixels and vice versa
      const int pad_rb_br = (passes == 1) ? 6 : 5;
      dt_opencl_local_buffer_t locopt_rb_br
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_red_and_blue, &locopt_rb_br))
      goto error;

      for(int d = 0; d < 4; d++)
      {
        size_t sizes[3] = { ROUNDUP(width, locopt_rb_br.sizex), ROUNDUP(height, locopt_rb_br.sizey), 1 };
        size_t local[3] = { locopt_rb_br.sizex, locopt_rb_br.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_red_and_blue, 0, CLARG(dev_rgb[d]), CLARG(width),
          CLARG(height), CLARG(pad_rb_br), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(d), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLLOCAL(sizeof(float) * 4 * (locopt_rb_br.sizex + 2*3) * (locopt_rb_br.sizey + 2*3)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_red_and_blue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue for 2x2 blocks of green
      const int pad_g22 = (passes == 1) ? 8 : 4;
      dt_opencl_local_buffer_t locopt_g22
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_interpolate_twoxtwo, &locopt_g22))
      goto error;

      for(int d = 0, n = 0; d < ndir; d += 2, n++)
      {
        size_t sizes[3] = { ROUNDUP(width, locopt_g22.sizex), ROUNDUP(height, locopt_g22.sizey), 1 };
        size_t local[3] = { locopt_g22.sizex, locopt_g22.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 0, CLARG(dev_rgb[n]),
          CLARG(width), CLARG(height), CLARG(pad_g22), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(d), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLARG(dev_allhex), CLLOCAL(sizeof(float) * 4 * (locopt_g22.sizex + 2*2) * (locopt_g22.sizey + 2*2)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_twoxtwo, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }
    // end of multi pass

    // gminmax data no longer needed
    dt_opencl_release_mem_object(dev_gminmax);
    dev_gminmax = NULL;

    // jump back to the first set of rgb buffers (this is a noop for Markesteijn-1)
    dev_rgb = dev_rgbv;

    // prepare derivatives buffers
    for(int n = 0; n < ndir; n++)
    {
      dev_drv[n] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
      if(dev_drv[n] == NULL) goto error;
    }

    // convert to perceptual colorspace and differentiate in all directions
    const int pad_yuv = (passes == 1) ? 8 : 13;
    dt_opencl_local_buffer_t locopt_diff
      = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_differentiate, &locopt_diff))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      // convert to perceptual YPbPr colorspace
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_convert_yuv, width, height,
        CLARG(dev_rgb[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_yuv));
      if(err != CL_SUCCESS) goto error;


      // differentiate in all directions
      size_t sizes_diff[3] = { ROUNDUP(width, locopt_diff.sizex), ROUNDUP(height, locopt_diff.sizey), 1 };
      size_t local_diff[3] = { locopt_diff.sizex, locopt_diff.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_differentiate, 0, CLARG(dev_aux), CLARG(dev_drv[d]),
        CLARG(width), CLARG(height), CLARG(pad_yuv), CLARG(d), CLLOCAL(sizeof(float) * 4 * (locopt_diff.sizex + 2*1) * (locopt_diff.sizey + 2*1)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_differentiate, sizes_diff, local_diff);
      if(err != CL_SUCCESS) goto error;
    }

    // reserve buffers for homogeneity maps and sum maps
    for(int n = 0; n < ndir; n++)
    {
      dev_homo[n] = dt_opencl_alloc_device_buffer(devid, sizeof(unsigned char) * width * height);
      if(dev_homo[n] == NULL) goto error;

      dev_homosum[n] = dt_opencl_alloc_device_buffer(devid, sizeof(unsigned char) * width * height);
      if(dev_homosum[n] == NULL) goto error;
    }

    // get thresholds for homogeneity map (store them in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      const int pad_homo = (passes == 1) ? 10 : 15;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_threshold, width, height,
        CLARG(dev_drv[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_homo), CLARG(d));
      if(err != CL_SUCCESS) goto error;
    }

    // set homogeneity maps
    const int pad_homo = (passes == 1) ? 10 : 15;
    dt_opencl_local_buffer_t locopt_homo
      = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_homo_set, &locopt_homo))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      size_t sizes[3] = { ROUNDUP(width, locopt_homo.sizex),ROUNDUP(height, locopt_homo.sizey), 1 };
      size_t local[3] = { locopt_homo.sizex, locopt_homo.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_homo_set, 0, CLARG(dev_drv[d]), CLARG(dev_aux),
        CLARG(dev_homo[d]), CLARG(width), CLARG(height), CLARG(pad_homo), CLLOCAL(sizeof(float) * (locopt_homo.sizex + 2*1) * (locopt_homo.sizey + 2*1)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_set, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // get rid of dev_drv buffers
    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_drv[n]);
      dev_drv[n] = NULL;
    }

    // build 5x5 sum of homogeneity maps for each pixel and direction
    dt_opencl_local_buffer_t locopt_homo_sum
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_homo_sum, &locopt_homo_sum))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      size_t sizes[3] = { ROUNDUP(width, locopt_homo_sum.sizex), ROUNDUP(height, locopt_homo_sum.sizey), 1 };
      size_t local[3] = { locopt_homo_sum.sizex, locopt_homo_sum.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_homo_sum, 0, CLARG(dev_homo[d]), CLARG(dev_homosum[d]),
        CLARG(width), CLARG(height), CLARG(pad_tile), CLLOCAL(sizeof(char) * (locopt_homo_sum.sizex + 2*2) * (locopt_homo_sum.sizey + 2*2)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_sum, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // get maximum of homogeneity maps (store in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_max, width, height,
        CLARG(dev_homosum[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_tile), CLARG(d));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // adjust maximum value
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_max_corr, width, height,
        CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    // for Markesteijn-3: use only one of two directions if there is a difference in homogeneity
    for(int d = 0; d < ndir - 4; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_quench, width, height,
        CLARG(dev_homosum[d]), CLARG(dev_homosum[d + 4]), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // initialize output buffer to zero
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_zero, width, height,
        CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    // need to get another temp buffer for the output image (may use the space of dev_drv[] freed earlier)
    dev_tmptmp = dt_opencl_alloc_device(devid, (size_t)width, height, sizeof(float) * 4);
    if(dev_tmptmp == NULL) goto error;

    cl_mem dev_t1 = dev_tmp;
    cl_mem dev_t2 = dev_tmptmp;

    // accumulate all contributions
    for(int d = 0; d < ndir; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_accu, width, height,
        CLARG(dev_t1), CLARG(dev_t2), CLARG(dev_rgbv[d]), CLARG(dev_homosum[d]), CLARG(dev_aux), CLARG(width),
        CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;

      // swap buffers
      cl_mem dev_t = dev_t2;
      dev_t2 = dev_t1;
      dev_t1 = dev_t;
    }

    // copy output to dev_tmptmp (if not already there)
    // note: we need to take swap of buffers into account, so current output lies in dev_t1
    if(dev_t1 != dev_tmptmp)
    {
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_t1, dev_tmptmp, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }

    {
      // process the final image
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_final, width, height,
        CLARG(dev_tmptmp), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(pad_tile), CLARRAY(4, processed_maximum));
      if(err != CL_SUCCESS) goto error;
    }

    // now it's time to get rid of most of the temporary buffers (except of dev_tmp and dev_xtrans)
    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_rgbv[n]);
      dev_rgbv[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_homo[n]);
      dev_homo[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_homosum[n]);
      dev_homosum[n] = NULL;
    }

    dt_opencl_release_mem_object(dev_aux);
    dev_aux = NULL;

    dt_opencl_release_mem_object(dev_xtrans);
    dev_xtrans = NULL;

    dt_opencl_release_mem_object(dev_allhex);
    dev_allhex = NULL;

    dt_opencl_release_mem_object(dev_green_eq);
    dev_green_eq = NULL;

    dt_opencl_release_mem_object(dev_tmptmp);
    dev_tmptmp = NULL;

    // take care of image borders. the algorithm above leaves an unprocessed border of pad_tile pixels.
    // strategy: take the four edges and process them each with process_vng_cl(). as VNG produces
    // an image with a border with only linear interpolation we process edges of pad_tile+3px and
    // drop 3px on the inner side if possible

    // take care of some degenerate cases (which might happen if we are called in a tiling context)
    const int wd = (width > pad_tile+3) ? pad_tile+3 : width;
    const int ht = (height > pad_tile+3) ? pad_tile+3 : height;
    const int wdc = (wd >= pad_tile+3) ? 3 : 0;
    const int htc = (ht >= pad_tile+3) ? 3 : 0;

    // the data of all four edges:
    // total edge: x-offset, y-offset, width, height,
    // after dropping: x-offset adjust, y-offset adjust, width adjust, height adjust
    const int edges[4][8] = { { 0, 0, wd, height, 0, 0, -wdc, 0 },
                              { 0, 0, width, ht, 0, 0, 0, -htc },
                              { width - wd, 0, wd, height, wdc, 0, -wdc, 0 },
                              { 0, height - ht, width, ht, 0, htc, 0, -htc } };

    for(int n = 0; n < 4; n++)
    {
      dt_iop_roi_t roi = { roi_in->x + edges[n][0], roi_in->y + edges[n][1], edges[n][2], edges[n][3], 1.0f };

      size_t iorigin[] = { edges[n][0], edges[n][1], 0 };
      size_t oorigin[] = { 0, 0, 0 };
      size_t region[] = { edges[n][2], edges[n][3], 1 };

      // reserve input buffer for image edge
      dev_edge_in = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], sizeof(float));
      if(dev_edge_in == NULL) goto error;

      // reserve output buffer for VNG processing of edge
      dev_edge_out = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], sizeof(float) * 4);
      if(dev_edge_out == NULL) goto error;

      // copy edge to input buffer
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_edge_in, iorigin, oorigin, region);
      if(err != CL_SUCCESS) goto error;

      // VNG processing
      if(!process_vng_cl(self, piece, dev_edge_in, dev_edge_out, &roi, &roi, smooth, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR))
        goto error;

      // adjust for "good" part, dropping linear border where possible
      iorigin[0] += edges[n][4];
      iorigin[1] += edges[n][5];
      oorigin[0] += edges[n][4];
      oorigin[1] += edges[n][5];
      region[0] += edges[n][6];
      region[1] += edges[n][7];

      // copy output
      err = dt_opencl_enqueue_copy_image(devid, dev_edge_out, dev_tmp, oorigin, iorigin, region);
      if(err != CL_SUCCESS) goto error;

      // release intermediate buffers
      dt_opencl_release_mem_object(dev_edge_in);
      dt_opencl_release_mem_object(dev_edge_out);
      dev_edge_in = dev_edge_out = NULL;
    }

    if(piece->pipe->want_detail_mask)
      dt_dev_write_rawdetail_mask_cl(piece, dev_tmp, roi_in, TRUE);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_tmp, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample third-size image
    const int width = roi_out->width;
    const int height = roi_out->height;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
      CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(dev_xtrans));
    if(err != CL_SUCCESS) goto error;
  }

  // free remaining temporary buffers
  if(dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;


  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);

  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_rgbv[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_drv[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_homo[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_homosum[n]);
  dt_opencl_release_mem_object(dev_gminmax);
  dt_opencl_release_mem_object(dev_tmptmp);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_allhex);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_edge_in);
  dt_opencl_release_mem_object(dev_edge_out);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue process_markesteijn_cl kernel! %s\n", cl_errstr(err));
  return FALSE;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

