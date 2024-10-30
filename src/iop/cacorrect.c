/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

// fast-math changes results enough to fail the integration test, and isn't even any faster...
#ifdef __GNUC__
#pragma GCC optimize ("no-fast-math")
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/gaussian.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(2, dt_iop_cacorrect_params_t)

//#pragma GCC diagnostic ignored "-Wshadow"

typedef enum dt_iop_cacorrect_multi_t
{
  CACORRETC_MULTI_1 = 1,     // $DESCRIPTION: "once"
  CACORRETC_MULTI_2 = 2,     // $DESCRIPTION: "twice"
  CACORRETC_MULTI_3 = 3,     // $DESCRIPTION: "three times"
  CACORRETC_MULTI_4 = 4,     // $DESCRIPTION: "four times"
  CACORRETC_MULTI_5 = 5,     // $DESCRIPTION: "five times"
} dt_iop_cacorrect_multi_t;

typedef struct dt_iop_cacorrect_params_t
{
  gboolean avoidshift;      // $DEFAULT: 0 $DESCRIPTION: "avoid colorshift"
  dt_iop_cacorrect_multi_t iterations; // $DEFAULT: CACORRETC_MULTI_2 $DESCRIPTION: "iterations"
} dt_iop_cacorrect_params_t;

typedef struct dt_iop_cacorrect_gui_data_t
{
  GtkWidget *avoidshift;
  GtkWidget *iterations;
} dt_iop_cacorrect_gui_data_t;

typedef struct dt_iop_cacorrect_data_t
{
  gboolean avoidshift;
  uint32_t iterations;
} dt_iop_cacorrect_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("raw chromatic aberrations");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct chromatic aberrations for Bayer sensors"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, raw, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_cacorrect_params_v2_t
  {
    gboolean avoidshift;
    dt_iop_cacorrect_multi_t iterations;
  } dt_iop_cacorrect_params_v2_t;

  if(old_version == 1)
  {
    dt_iop_cacorrect_params_v2_t *n = malloc(sizeof(dt_iop_cacorrect_params_v2_t));
    n->avoidshift = FALSE;
    n->iterations = 1;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_cacorrect_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

/*==================================================================================
 * begin raw therapee code, hg initial checkout of march 09, 2016 branch master.
 * avoid colorshift code has been added later
 *==================================================================================*/

////////////////////////////////////////////////////////////////
//
//  Chromatic Aberration correction on raw bayer cfa data
//
//		copyright (c) 2008-2010  Emil Martinec <ejmartin@uchicago.edu>
//  copyright (c) for improvements (speedups, iterated correction and avoid colour shift) 2018 Ingo Weyrich <heckflosse67@gmx.de>
//
//  code dated: September 8, 2018
//
//	CA_correct_RT.cc is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//      along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
static gboolean _LinEqSolve(ssize_t nDim, double *pfMatr, double *pfVect, double *pfSolution)
{
  //==============================================================================
  // return 1 if system not solving, 0 if system solved
  // nDim - system dimension
  // pfMatr - matrix with coefficients
  // pfVect - vector with free members
  // pfSolution - vector with system solution
  // pfMatr becomes triangular after function call
  // pfVect changes after function call
  //
  // Developer: Henry Guennadi Levkin
  //
  //==============================================================================

  double fMaxElem;
  double fAcc;

  ssize_t i, j, k, m;

  for(k = 0; k < (nDim - 1); k++)
  { // base row of matrix
    // search of line with max element
    fMaxElem = fabs(pfMatr[k * nDim + k]);
    m = k;

    for(i = k + 1; i < nDim; i++)
    {
      if(fMaxElem < fabs(pfMatr[i * nDim + k]))
      {
        fMaxElem = pfMatr[i * nDim + k];
        m = i;
      }
    }

    // permutation of base line (index k) and max element line(index m)
    if(m != k)
    {
      for(i = k; i < nDim; i++)
      {
        fAcc = pfMatr[k * nDim + i];
        pfMatr[k * nDim + i] = pfMatr[m * nDim + i];
        pfMatr[m * nDim + i] = fAcc;
      }

      fAcc = pfVect[k];
      pfVect[k] = pfVect[m];
      pfVect[m] = fAcc;
    }

    if(pfMatr[k * nDim + k] == 0.)
    {
      // linear system has no solution
      return FALSE; // needs improvement !!!
    }

    // triangulation of matrix with coefficients
    for(j = (k + 1); j < nDim; j++)
    { // current row of matrix
      fAcc = -pfMatr[j * nDim + k] / pfMatr[k * nDim + k];

      for(i = k; i < nDim; i++)
      {
        pfMatr[j * nDim + i] = pfMatr[j * nDim + i] + fAcc * pfMatr[k * nDim + i];
      }

      pfVect[j] = pfVect[j] + fAcc * pfVect[k]; // free member recalculation
    }
  }

  for(k = (nDim - 1); k >= 0; k--)
  {
    pfSolution[k] = pfVect[k];

    for(i = (k + 1); i < nDim; i++)
    {
      pfSolution[k] -= (pfMatr[k * nDim + i] * pfSolution[i]);
    }

    pfSolution[k] = pfSolution[k] / pfMatr[k * nDim + k];
  }

  return TRUE;
}
// end of linear equation solver
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const float *const input = (float *)ivoid;
  float *output = (float *) ovoid;

  const uint32_t filters = piece->pipe->dsc.filters;

  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  dt_iop_cacorrect_data_t *d = piece->data;

  // the colorshift avoiding requires non-downscaled data for sure so we
  // don't do this for preview
  const gboolean avoidshift = d->avoidshift && !(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW);
  const int iterations = d->iterations;

  // Because we can't break parallel processing, we need a switch do handle the errors
  gboolean processpasstwo = TRUE;

  float *redfactor = NULL;
  float *bluefactor = NULL;
  float *oldraw = NULL;
  float *blockwt = NULL;
  float *RawDataTmp = NULL;
  float *Gtmp = NULL;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t ibsize = dt_round_size(width, DT_CACHELINE_FLOATS) * (size_t)(height + 2);

  const int h_width = (width + 1) / 2;
  const int h_height = (height + 1) / 2;
  const size_t h_bsize = dt_round_size(h_width, DT_CACHELINE_FLOATS) * (size_t)(h_height + 2);

  float *out = dt_alloc_align_float(ibsize);
  if(!out)
  {
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out);
    dt_print(DT_DEBUG_ALWAYS,"[cacorrect] out of memory, skipping");
    return;
  }

  const float scaler = dt_iop_get_processed_maximum(piece);
  dt_iop_image_scaled_copy(out, input, 1.0f / scaler, width, height, 1);

  if(run_fast) goto writeout;


  const float *const in = out;

  #define caautostrength 4.0f
  #define ts 128    // multiple of 16 for aligned buffers
  #define tsh (ts / 2)
  #define v1 (ts)
  #define v2 (2 * ts)
  #define v3 (3 * ts)
  #define v4 (4 * ts)
  #define border 8
  #define border2 (2 * border)
  #define borderh (border / 2)

  // multithreaded and partly vectorized by Ingo Weyrich

  if(avoidshift)
  {
    redfactor = dt_calloc_align_float(h_bsize);
    bluefactor = dt_calloc_align_float(h_bsize);
    oldraw = dt_calloc_align_float(h_bsize * 2);
    if(!redfactor || !bluefactor || !oldraw)
    {
      dt_print(DT_DEBUG_ALWAYS,"[cacorrect] out of memory, skipping");
      goto writeout;
    }
    // copy raw values before ca correction
    DT_OMP_FOR()
    for(size_t row = 0; row < height; row++)
    {
      for(size_t col = (FC(row, 0, filters) & 1); col < width; col += 2)
      {
        oldraw[row * h_width + col / 2] = in[row * width + col];
      }
    }
  }

  double fitparams[2][2][16];

  // temporary array to store simple interpolation of G
  Gtmp = dt_calloc_align_float(ibsize);

  // temporary array to avoid race conflicts, only every second pixel needs to be saved here
  RawDataTmp = dt_alloc_align_float(ibsize / 2);

  if(!Gtmp || !RawDataTmp)
  {
    dt_print(DT_DEBUG_ALWAYS,"[cacorrect] out of memory, skipping");
    goto writeout;
  }

  const int vz1 = (height + border2) % (ts - border2) == 0 ? 1 : 0;
  const int hz1 = (width + border2) % (ts - border2) == 0 ? 1 : 0;
  const int vert_tiles = ceilf((float)(height + border2) / (ts - border2) + 2 + vz1);
  const int horiz_tiles = ceilf((float)(width + border2) / (ts - border2) + 2 + hz1);

  // block CA shift values and weight assigned to block
  blockwt = dt_calloc_align_float(5 * vert_tiles * horiz_tiles);
  float (*blockshifts)[2][2] = (float(*)[2][2])(blockwt + vert_tiles * horiz_tiles);

  float blockave[2][2] = { { 0, 0 }, { 0, 0 } };
  float blocksqave[2][2] = { { 0, 0 }, { 0, 0 } };
  float blockdenom[2][2] = { { 0, 0 }, { 0, 0 } };
  float blockvar[2][2];
  // order of 2d polynomial fit (polyord), and numpar=polyord^2
  int polyord = 4;
  int numpar = 16;

  const float eps = 1e-5f;
  const float eps2 = 1e-10f; // tolerance to avoid dividing by zero

  for(int it = 0; it < iterations && processpasstwo; it++)
  {
    // A reminder, be very careful if you try to optimize the parallel processing loop
    // for not breaking multipass mode and clang/gcc differences.
    // See darktable file history and related problems before doing so.
    DT_OMP_PRAGMA(parallel)
    {
      // direction of the CA shift in a tile
      int GRBdir[2][3];

      int shifthfloor[3];
      int shiftvfloor[3];
      int shifthceil[3];
      int shiftvceil[3];

      // local quadratic fit to shift data within a tile
      float coeff[2][3][2];
      // measured CA shift parameters for a tile
      float CAshift[2][2];
      // polynomial fit coefficients
      // residual CA shift amount within a plaquette
      float shifthfrac[3];
      float shiftvfrac[3];
      // per thread data for evaluation of block CA shift variance
      float blockavethr[2][2] = { { 0, 0 }, { 0, 0 } };
      float blocksqavethr[2][2] = { { 0, 0 }, { 0, 0 } };
      float blockdenomthr[2][2] = { { 0, 0 }, { 0, 0 } };

      // assign working space
      // allocate all buffers in one bunch but make sure they are all aligned-64 by proper tilesize ts
      const int tilebuf_size = ts * ts;
      const int tilebuf_half_size = ts * tsh;

      const size_t buffersize = 3 * (size_t)tilebuf_size + 6 * tilebuf_half_size;
      float *data = dt_alloc_align_float(buffersize);

      // rgb data in a tile
      float *rgb[3];
      rgb[0] = (float(*))data;
      rgb[1] = (float(*))(data + tilebuf_size);
      rgb[2] = (float(*))(data + 2 * tilebuf_size);

      // high pass filter for R/B in vertical direction
      float *rbhpfh = data + 3 * tilebuf_size;
      // high pass filter for R/B in horizontal direction
      float *rbhpfv = data + 3 * tilebuf_size + 1 * tilebuf_half_size;
      // low pass filter for R/B in horizontal direction
      float *rblpfh = data + 3 * tilebuf_size + 2 * tilebuf_half_size;
      // low pass filter for R/B in vertical direction
      float *rblpfv = data + 3 * tilebuf_size + 3 * tilebuf_half_size;
      // low pass filter for colour differences in horizontal direction
      float *grblpfh = data + 3 * tilebuf_size + 4 * tilebuf_half_size;
      // low pass filter for colour differences in vertical direction
      float *grblpfv = data + 3 * tilebuf_size + 5 * tilebuf_half_size;
      // colour differences
      float *grbdiff = rbhpfh; // there is no overlap in buffer usage => share
      // green interpolated to optical sample points for R/B
      float *gshift = rbhpfv; // there is no overlap in buffer usage => share

// Main algorithm: Tile loop calculating correction parameters per tile
      DT_OMP_PRAGMA(for collapse(2) schedule(static) nowait)
      for(int top = -border; top < height; top += ts - border2)
      {
        for(int left = -border; left < width; left += ts - border2)
        {
          memset(data, 0, buffersize * sizeof(float));
          const int vblock = ((top + border) / (ts - border2)) + 1;
          const int hblock = ((left + border) / (ts - border2)) + 1;
          const int bottom = MIN(top + ts, height + border);
          const int right = MIN(left + ts, width + border);
          const int rr1 = bottom - top;
          const int cc1 = right - left;
          const int rrmin = top < 0 ? border : 0;
          const int rrmax = bottom > height ? height - top : rr1;
          const int ccmin = left < 0 ? border : 0;
          const int ccmax = right > width ? width - left : cc1;

          // rgb from input CFA data
          // rgb values should be floating point numbers between 0 and 1
          // after white balance multipliers are applied

          for(int rr = rrmin; rr < rrmax; rr++)
          {
            int row = rr + top;
            int c = FC(rr, ccmin, filters);
            const int c_diff = c ^ FC(rr, ccmin+1, filters);
            for(int cc = ccmin; cc < ccmax; cc++)
            {
              const int col = cc + left;
              const size_t indx = (size_t)row * width + col;
              const size_t indx1 = (size_t)rr * ts + cc;
              rgb[c][indx1] = in[indx];
              c ^= c_diff;
            }
          }
          // fill borders
          if(rrmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][(border2 - rr) * ts + cc];
             }
          }
          if(rrmax < rr1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = in[(height - rr - 2) * width + left + cc];
              }
          }
          if(ccmin > 0)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][rr * ts + border2 - cc];
              }
          }
          if(ccmax < cc1)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][rr * ts + ccmax + cc] = in[(top + rr) * width + (width - cc - 2)];
              }
          }
          // also, fill the image corners
          if(rrmin > 0 && ccmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + cc] = in[(border2 - rr) * width + border2 - cc];
              }
          }
          if(rrmax < rr1 && ccmax < cc1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + ccmax + cc] = in[(height - rr - 2) * width + (width - cc - 2)];
              }
          }
          if(rrmin > 0 && ccmax < cc1)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + ccmax + cc] = in[(border2 - rr) * width + (width - cc - 2)];
              }
          }
          if(rrmax < rr1 && ccmin > 0)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < border; cc++)
              {
                const int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = in[(height - rr - 2) * width + (border2 - cc)];
              }
          }
          // end of border fill
          // end of initialization
            for(int rr = 3; rr < rr1 - 3; rr++)
          {
            int row = rr + top;
            for(int cc = 3 + (FC(rr, 3, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters);
                  cc < cc1 - 3;
                  cc += 2, indx += 2)
            {
              // compute directional weights using image gradients
              const float wtu = 1.f / sqrf(eps+ fabsf(rgb[1][indx + v1] - rgb[1][indx - v1])
                                              + fabsf(rgb[c][indx]      - rgb[c][indx - v2])
                                              + fabsf(rgb[1][indx - v1] - rgb[1][indx - v3]));
              const float wtd = 1.f / sqrf(eps+ fabsf(rgb[1][indx - v1] - rgb[1][indx + v1])
                                              + fabsf(rgb[c][indx]      - rgb[c][indx + v2])
                                              + fabsf(rgb[1][indx + v1] - rgb[1][indx + v3]));
              const float wtl = 1.f / sqrf(eps+ fabsf(rgb[1][indx + 1]  - rgb[1][indx - 1])
                                              + fabsf(rgb[c][indx]      - rgb[c][indx - 2])
                                              + fabsf(rgb[1][indx - 1]  - rgb[1][indx - 3]));
              const float wtr = 1.f / sqrf(eps+ fabsf(rgb[1][indx - 1]  - rgb[1][indx + 1])
                                              + fabsf(rgb[c][indx]      - rgb[c][indx + 2])
                                              + fabsf(rgb[1][indx + 1]  - rgb[1][indx + 3]));
              // store in rgb array the interpolated G value at R/B grid points using directional weighted average
              rgb[1][indx] = (wtu * rgb[1][indx - v1] + wtd * rgb[1][indx + v1] + wtl * rgb[1][indx - 1] + wtr * rgb[1][indx + 1])
                              / (wtu + wtd + wtl + wtr);
            }
            if(row > -1 && row < height)
            {
              for(int col = MAX(left + 3, 0), indx = rr * ts + 3 - (left < 0 ? (left + 3) : 0);
                    col < MIN(cc1 + left - 3, width);
                    col++, indx++)
              {
                Gtmp[row * width + col] = rgb[1][indx];
              }
            }
          }
          for(int rr = borderh; rr < rr1 - borderh; rr++)
          {
            for(int cc = borderh + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters);
                  cc < cc1 - borderh;
                  cc += 2, indx += 2)
            {
              rbhpfv[indx >> 1] = fabsf(fabsf((rgb[1][indx] - rgb[c][indx])           - (rgb[1][indx + v4] - rgb[c][indx + v4]))
                                      + fabsf((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx] - rgb[c][indx]))
                                      - fabsf((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx + v4] - rgb[c][indx + v4])));
              rbhpfh[indx >> 1] = fabsf(fabsf((rgb[1][indx] - rgb[c][indx])           - (rgb[1][indx + 4] - rgb[c][indx + 4]))
                                      + fabsf((rgb[1][indx - 4] - rgb[c][indx - 4])   - (rgb[1][indx] - rgb[c][indx]))
                                      - fabsf((rgb[1][indx - 4] - rgb[c][indx - 4])   - (rgb[1][indx + 4] - rgb[c][indx + 4])));
                // low and high pass 1D filters of G in vertical/horizontal directions
              const float glpfv = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + v2] + rgb[1][indx - v2]);
              const float glpfh = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + 2] + rgb[1][indx - 2]);
              rblpfv[indx >> 1] = eps + fabsf(glpfv - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]));
              rblpfh[indx >> 1] = eps + fabsf(glpfh - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]));
              grblpfv[indx >> 1]= glpfv + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]);
              grblpfh[indx >> 1] = glpfh + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]);
            }
          }
          for(int dir = 0; dir < 2; dir++)
          {
            for(int k = 0; k < 3; k++)
            {
              for(int c = 0; c < 2; c++)
                coeff[dir][k][c] = 0;
            }
          }

          // along line segments, find the point along each segment that minimizes the colour variance
          // averaged over the tile; evaluate for up/down and left/right away from R/B grid point
          for(int rr = border; rr < rr1 - border; rr++)
          {
            for(int cc = border + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters);
                  cc < cc1 - border;
                  cc += 2, indx += 2)
            {
              // in linear interpolation, colour differences are a quadratic function of interpolation
              // position; solve for the interpolation position that minimizes colour difference variance over the tile
                // vertical
              float gdiff = 0.3125f * (rgb[1][indx + ts] - rgb[1][indx - ts])
                         + 0.09375f * (rgb[1][indx + ts + 1] - rgb[1][indx - ts + 1] + rgb[1][indx + ts - 1] - rgb[1][indx - ts - 1]);
              float deltgrb = (rgb[c][indx] - rgb[1][indx]);
              float gradwt = fabsf(0.25f *  rbhpfv[indx >> 1] + 0.125f * (rbhpfv[(indx >> 1) + 1] + rbhpfv[(indx >> 1) - 1]))
                             * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1])
                             / (eps + 0.1f * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1]) + rblpfv[(indx >> 1) - v1] + rblpfv[(indx >> 1) + v1]);
              coeff[0][0][c >> 1] += gradwt * deltgrb * deltgrb;
              coeff[0][1][c >> 1] += gradwt * gdiff * deltgrb;
              coeff[0][2][c >> 1] += gradwt * gdiff * gdiff;
                // horizontal
              gdiff = 0.3125f * (rgb[1][indx + 1] - rgb[1][indx - 1])
                   + 0.09375f * (rgb[1][indx + 1 + ts] - rgb[1][indx - 1 + ts] + rgb[1][indx + 1 - ts] - rgb[1][indx - 1 - ts]);
              gradwt = fabsf(0.25f * rbhpfh[indx >> 1] + 0.125f * (rbhpfh[(indx >> 1) + v1] + rbhpfh[(indx >> 1) - v1]))
                       * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1])
                       / (eps + 0.1f * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1]) + rblpfh[(indx >> 1) - 1] + rblpfh[(indx >> 1) + 1]);
              coeff[1][0][c >> 1] += gradwt * deltgrb * deltgrb;
              coeff[1][1][c >> 1] += gradwt * gdiff * deltgrb;
              coeff[1][2][c >> 1] += gradwt * gdiff * gdiff;
              //  In Mathematica,
              //  f[x_]=Expand[Total[Flatten[
              //  ((1-x) RotateLeft[Gint,shift1]+x
              //  RotateLeft[Gint,shift2]-cfapad)^2[[dv;;-1;;2,dh;;-1;;2]]]]];
              //  extremum = -.5Coefficient[f[x],x]/Coefficient[f[x],x^2]
            }
          }

          for(int c = 0; c < 2; c++)
          {
            for(int dir = 0; dir < 2; dir++)
            { // vert/hor
              // CAshift[dir][c] are the locations
              // that minimize colour difference variances;
              // This is the approximate _optical_ location of the R/B pixels
              if(coeff[dir][2][c] > eps2)
              {
                CAshift[dir][c] = coeff[dir][1][c] / coeff[dir][2][c];
                blockwt[vblock * horiz_tiles + hblock] = coeff[dir][2][c] / (eps + coeff[dir][0][c]);
              }
              else
              {
                CAshift[dir][c] = 17.0f;
                blockwt[vblock * horiz_tiles + hblock] = 0;
              }
              // data structure = CAshift[vert/hor][colour]
              // dir : 0=vert, 1=hor
              // offset gives NW corner of square containing the min; dir : 0=vert, 1=hor
              if(fabsf(CAshift[dir][c]) < 2.0f)
              {
                blockavethr[dir][c] += CAshift[dir][c];
                blocksqavethr[dir][c] += sqrf(CAshift[dir][c]);
                blockdenomthr[dir][c] += 1;
              }
              // evaluate the shifts to the location that minimizes CA within the tile
              blockshifts[vblock * horiz_tiles + hblock][c][dir] = CAshift[dir][c]; // vert/hor CA shift for R/B
            } // vert/hor
          }   // colour
        }
      }
      // end of diagnostic pass per tile

      DT_OMP_PRAGMA(critical(cadetectpass2))
      {
        for(int dir = 0; dir < 2; dir++)
          for(int c = 0; c < 2; c++)
          {
            blockdenom[dir][c] += blockdenomthr[dir][c];
            blocksqave[dir][c] += blocksqavethr[dir][c];
            blockave[dir][c] += blockavethr[dir][c];
          }
      }

      DT_OMP_PRAGMA(barrier)

      DT_OMP_PRAGMA(single)
      {
        for(int dir = 0; dir < 2; dir++)
        {
          for(int c = 0; c < 2; c++)
          {
            if(blockdenom[dir][c])
              blockvar[dir][c] = blocksqave[dir][c] / blockdenom[dir][c] - sqrf(blockave[dir][c] / blockdenom[dir][c]);
            else
            {
              processpasstwo = FALSE;
              dt_print(DT_DEBUG_PIPE, "[cacorrect] blockdenom vanishes");
              break;
            }
          }
        }
        // now prepare for CA correction pass
        // first, fill border blocks of blockshift array
        if(processpasstwo)
        {
          for(int vblock = 1; vblock < vert_tiles - 1; vblock++)
          { // left and right sides
            for(int c = 0; c < 2; c++)
            {
              for(int i = 0; i < 2; i++)
              {
                blockshifts[vblock * horiz_tiles][c][i] = blockshifts[(vblock)*horiz_tiles + 2][c][i];
                blockshifts[vblock * horiz_tiles + horiz_tiles - 1][c][i] = blockshifts[(vblock)*horiz_tiles + horiz_tiles - 3][c][i];
              }
            }
          }
          for(int hblock = 0; hblock < horiz_tiles; hblock++)
          { // top and bottom sides
            for(int c = 0; c < 2; c++)
            {
              for(int i = 0; i < 2; i++)
              {
                blockshifts[hblock][c][i] = blockshifts[2 * horiz_tiles + hblock][c][i];
                blockshifts[(vert_tiles - 1) * horiz_tiles + hblock][c][i] = blockshifts[(vert_tiles - 3) * horiz_tiles + hblock][c][i];
              }
            }
          }
          // end of filling border pixels of blockshift array
          // initialize fit arrays
          double polymat[2][2][256];
          double shiftmat[2][2][16];

          for(int i = 0; i < 256; i++)
            polymat[0][0][i] = polymat[0][1][i] = polymat[1][0][i] = polymat[1][1][i] = 0;

          for(int i = 0; i < 16; i++)
            shiftmat[0][0][i] = shiftmat[0][1][i] = shiftmat[1][0][i] = shiftmat[1][1][i] = 0;

          int numblox[2] = { 0, 0 };

          for(int vblock = 1; vblock < vert_tiles - 1; vblock++)
          {
            for(int hblock = 1; hblock < horiz_tiles - 1; hblock++)
            {
              // block 3x3 median of blockshifts for robustness
              for(int c = 0; c < 2; c++)
              {
                float bstemp[2];
                for(int dir = 0; dir < 2; dir++)
                {
                  const float p[9]  __attribute__((aligned(16))) =
                        { blockshifts[(vblock - 1) * horiz_tiles + hblock - 1][c][dir],
                          blockshifts[(vblock - 1) * horiz_tiles + hblock    ][c][dir],
                          blockshifts[(vblock - 1) * horiz_tiles + hblock + 1][c][dir],
                          blockshifts[(vblock)     * horiz_tiles + hblock - 1][c][dir],
                          blockshifts[(vblock)     * horiz_tiles + hblock    ][c][dir],
                          blockshifts[(vblock)     * horiz_tiles + hblock + 1][c][dir],
                          blockshifts[(vblock + 1) * horiz_tiles + hblock - 1][c][dir],
                          blockshifts[(vblock + 1) * horiz_tiles + hblock    ][c][dir],
                          blockshifts[(vblock + 1) * horiz_tiles + hblock + 1][c][dir] };
                  bstemp[dir] = median9f(p);
                }
                  // now prepare coefficient matrix; use only data points within caautostrength/2 std devs of zero
                if(sqrf(bstemp[0]) > caautostrength * blockvar[0][c] || sqrf(bstemp[1]) > caautostrength * blockvar[1][c])
                  continue;

                numblox[c]++;
                double powVblockInit = 1.0;
                for(int i = 0; i < polyord; i++)
                {
                  double powHblockInit = 1.0;
                  for(int j = 0; j < polyord; j++)
                  {
                    double powVblock = powVblockInit;
                    for(int m = 0; m < polyord; m++)
                    {
                      double powHblock = powHblockInit;
                      for(int n = 0; n < polyord; n++)
                      {
                        double inc = powVblock * powHblock * blockwt[vblock * horiz_tiles + hblock];
                        size_t idx = numpar * (polyord * i + j) + (polyord * m + n);
                        polymat[c][0][idx] += inc;
                        polymat[c][1][idx] += inc;
                        powHblock *= hblock;
                      }
                      powVblock *= vblock;
                    }
                    double blkinc = powVblockInit * powHblockInit * blockwt[vblock * horiz_tiles + hblock];
                    shiftmat[c][0][(polyord * i + j)] += blkinc * bstemp[0];
                    shiftmat[c][1][(polyord * i + j)] += blkinc * bstemp[1];
                    powHblockInit *= hblock;
                  }
                  powVblockInit *= vblock;
                }   // monomials
              }     // c
            }       // blocks
          }

          numblox[1] = MIN(numblox[0], numblox[1]);
          // if too few data points, restrict the order of the fit to linear
          if(numblox[1] < 32)
          {
            polyord = 2;
            numpar = 4;

            if(numblox[1] < 10)
            {
              dt_print(DT_DEBUG_PIPE, "[cacorrect] restrict fit to linear, numblox = %d ", numblox[1]);
              processpasstwo = FALSE;
            }
          }

          if(processpasstwo)
          {
            // fit parameters to blockshifts
            for(int c = 0; c < 2; c++)
              for(int dir = 0; dir < 2; dir++)
              {
                if(!_LinEqSolve(numpar, polymat[c][dir], shiftmat[c][dir], fitparams[c][dir]))
                {
                  dt_print(DT_DEBUG_PIPE,
                         "[cacorrect] can't solve linear equations for colour %d direction %d", c, dir);
                  processpasstwo = FALSE;
                }
              }
          }
        }
        // fitparams[polyord*i+j] gives the coefficients of (vblock^i hblock^j) in a polynomial fit for i,j<=4
      }
      // end of initialization for CA correction pass
      // only executed if cared and cablue are zero

      // Main algorithm: Tile loop
      if(processpasstwo)
      {
        DT_OMP_PRAGMA(for schedule(static) collapse(2) nowait)
        for(int top = -border; top < height; top += ts - border2)
        {
          for(int left = -border; left < width; left += ts - border2)
          {
            memset(data, 0, buffersize * sizeof(float));
            float lblockshifts[2][2];
            const int vblock = ((top + border) / (ts - border2)) + 1;
            const int hblock = ((left + border) / (ts - border2)) + 1;
            const int bottom = MIN(top + ts, height + border);
            const int right = MIN(left + ts, width + border);
            const int rr1 = bottom - top;
            const int cc1 = right - left;

            const int rrmin = top < 0 ? border : 0;
            const int rrmax = bottom > height ? height - top : rr1;
            const int ccmin = left < 0 ? border : 0;
            const int ccmax = right > width ? width - left : cc1;

            // rgb from input CFA data
            // rgb values should be floating point number between 0 and 1
            // after white balance multipliers are applied

            for(int rr = rrmin; rr < rrmax; rr++)
            {
              int row = rr + top;
              int c = FC(rr, ccmin, filters);
              const int c_diff = c ^ FC(rr, ccmin+1, filters);
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                const int col = cc + left;
                const size_t indx = (size_t)row * width + col;
                const size_t indx1 = (size_t)rr * ts + cc;
                rgb[c][indx1] = in[indx];

                if((c & 1) == 0)
                  rgb[1][indx1] = Gtmp[indx];
                c ^= c_diff;
              }
            }

            // fill borders
            if(rrmin > 0)
            {
              for(int rr = 0; rr < border; rr++)
                for(int cc = ccmin; cc < ccmax; cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][rr * ts + cc] = rgb[c][(border2 - rr) * ts + cc];
                  rgb[1][rr * ts + cc] = rgb[1][(border2 - rr) * ts + cc];
                }
            }
            if(rrmax < rr1)
            {
              for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
                for(int cc = ccmin; cc < ccmax; cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][(rrmax + rr) * ts + cc] = in[(height - rr - 2) * width + left + cc];
                  rgb[1][(rrmax + rr) * ts + cc] = Gtmp[(height - rr - 2) * width + left + cc];
                }
            }
            if(ccmin > 0)
            {
              for(int rr = rrmin; rr < rrmax; rr++)
                for(int cc = 0; cc < border; cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][rr * ts + cc] = rgb[c][rr * ts + border2 - cc];
                  rgb[1][rr * ts + cc] = rgb[1][rr * ts + border2 - cc];
                }
            }
            if(ccmax < cc1)
            {
              for(int rr = rrmin; rr < rrmax; rr++)
                for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][rr * ts + ccmax + cc] = in[(top + rr) * width + (width - cc - 2)];
                  rgb[1][rr * ts + ccmax + cc] = Gtmp[(top + rr) * width + (width - cc - 2)];
                }
            }
            // also, fill the image corners
            if(rrmin > 0 && ccmin > 0)
            {
              for(int rr = 0; rr < border; rr++)
                for(int cc = 0; cc < border; cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][(rr)*ts + cc] = in[(border2 - rr) * width + border2 - cc];
                  rgb[1][(rr)*ts + cc] = Gtmp[(border2 - rr) * width + border2 - cc];
                }
            }
            if(rrmax < rr1 && ccmax < cc1)
            {
              for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
                for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][(rrmax + rr) * ts + ccmax + cc] = in[(height - rr - 2) * width + (width - cc - 2)];
                  rgb[1][(rrmax + rr) * ts + ccmax + cc] = Gtmp[(height - rr - 2) * width + (width - cc - 2)];
                }
            }
            if(rrmin > 0 && ccmax < cc1)
            {
              for(int rr = 0; rr < border; rr++)
                for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][(rr)*ts + ccmax + cc] = in[(border2 - rr) * width + (width - cc - 2)];
                  rgb[1][(rr)*ts + ccmax + cc] = Gtmp[(border2 - rr) * width + (width - cc - 2)];
                }
            }
           if(rrmax < rr1 && ccmin > 0)
            {
              for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
                for(int cc = 0; cc < border; cc++)
                {
                  const int c = FC(rr, cc, filters);
                  rgb[c][(rrmax + rr) * ts + cc] = in[(height - rr - 2) * width + (border2 - cc)];
                  rgb[1][(rrmax + rr) * ts + cc] = Gtmp[(height - rr - 2) * width + (border2 - cc)];
                }
            }
            // end of border fill
            {
              // CA auto correction; use CA diagnostic pass to set shift parameters
              lblockshifts[0][0] = lblockshifts[0][1] = 0;
              lblockshifts[1][0] = lblockshifts[1][1] = 0;
              float powVblock = 1.0f;
              for(int i = 0; i < polyord; i++)
              {
                float powHblock = powVblock;
                for(int j = 0; j < polyord; j++)
                {
                  // printf("i= %d j= %d polycoeff= %f ",i,j,fitparams[0][0][polyord*i+j]);
                  lblockshifts[0][0] += powHblock * fitparams[0][0][polyord * i + j];
                  lblockshifts[0][1] += powHblock * fitparams[0][1][polyord * i + j];
                  lblockshifts[1][0] += powHblock * fitparams[1][0][polyord * i + j];
                  lblockshifts[1][1] += powHblock * fitparams[1][1][polyord * i + j];
                  powHblock *= hblock;
                }
                powVblock *= vblock;
              }
              const float bslim = 3.99f; // max allowed CA shift
              lblockshifts[0][0] = CLAMPF(lblockshifts[0][0], -bslim, bslim);
              lblockshifts[0][1] = CLAMPF(lblockshifts[0][1], -bslim, bslim);
              lblockshifts[1][0] = CLAMPF(lblockshifts[1][0], -bslim, bslim);
              lblockshifts[1][1] = CLAMPF(lblockshifts[1][1], -bslim, bslim);
            } // end of setting CA shift parameters

            for(int c = 0; c < 3; c += 2)
            {
              // some parameters for the bilinear interpolation
              shiftvfloor[c] = floorf(lblockshifts[c >> 1][0]);
              shiftvceil[c] = ceilf(lblockshifts[c >> 1][0]);
              if(lblockshifts[c>>1][0] < 0.f)
              {
                const int tmp = shiftvfloor[c];
                shiftvfloor[c] = shiftvceil[c];
                shiftvceil[c] = tmp;
              }
              shiftvfrac[c] = fabsf(lblockshifts[c>>1][0] - shiftvfloor[c]);
              shifthfloor[c] = floorf(lblockshifts[c >> 1][1]);
              shifthceil[c] = ceilf(lblockshifts[c >> 1][1]);
              if(lblockshifts[c>>1][1] < 0.f)
              {
                const int tmp = shifthfloor[c];
                shifthfloor[c] = shifthceil[c];
                shifthceil[c] = tmp;
              }
              shifthfrac[c] = fabsf(lblockshifts[c>>1][1] - shifthfloor[c]);

              GRBdir[0][c] = lblockshifts[c >> 1][0] > 0 ? 2 : -2;
              GRBdir[1][c] = lblockshifts[c >> 1][1] > 0 ? 2 : -2;
            }


            for(int rr = borderh; rr < rr1 - borderh; rr++)
            {
              for(int cc = borderh + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters); cc < cc1 - borderh; cc += 2)
              {
                // perform CA correction using colour ratios or colour differences
                const float Ginthfloor = interpolatef(shifthfrac[c],
                                                      rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthceil[c]],
                                                      rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthfloor[c]]);
                const float Ginthceil = interpolatef(shifthfrac[c],
                                                      rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthceil[c]],
                                                      rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthfloor[c]]);
                // Gint is bilinear interpolation of G at CA shift point
                const float Gint = interpolatef(shiftvfrac[c], Ginthceil, Ginthfloor);

                // determine R/B at grid points using colour differences at shift point plus interpolated G value at grid point
                // but first we need to interpolate G-R/G-B to grid points...
                grbdiff[(rr*ts + cc) >> 1] = Gint - rgb[c][rr*ts + cc];
                gshift[(rr*ts + cc) >> 1] = Gint;
              }
            }

            shifthfrac[0] /= 2.f;
            shifthfrac[2] /= 2.f;
            shiftvfrac[0] /= 2.f;
            shiftvfrac[2] /= 2.f;

            // this loop does not deserve vectorization in mainly because the most expensive part with the
            // divisions does not happen often (less than 1/10 in my tests)
            for(int rr = border; rr < rr1 - border; rr++)
            {
              for(int cc = border + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters), indx = rr * ts + cc;
                      cc < cc1 - border;
                      cc += 2, indx += 2)
              {
                const float grbdiffold = rgb[1][indx] - rgb[c][indx];
                // interpolate colour difference from optical R/B locations to grid locations
                const float grbdiffinthfloor = interpolatef(shifthfrac[c],
                                                            grbdiff[(indx - GRBdir[1][c]) >> 1],
                                                            grbdiff[indx >> 1]);
                const float grbdiffinthceil  = interpolatef(shifthfrac[c],
                                                            grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1],
                                                            grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1]);
                // grbdiffint is bilinear interpolation of G-R/G-B at grid point
                float grbdiffint = interpolatef(shiftvfrac[c], grbdiffinthceil, grbdiffinthfloor);

                // now determine R/B at grid points using interpolated colour differences and interpolated G value at grid point
                const float RBint = rgb[1][indx] - grbdiffint;
                if(fabsf(RBint - rgb[c][indx]) < 0.25f * (RBint + rgb[c][indx]))
                {
                  if(fabsf(grbdiffold) > fabsf(grbdiffint))
                    rgb[c][indx] = RBint;
                }
                else
                {
                  // gradient weights using difference from G at CA shift points and G at grid points
                  const float p0 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[indx >> 1]));
                  const float p1 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[(indx - GRBdir[1][c]) >> 1]));
                  const float p2 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[((rr - GRBdir[0][c]) * ts + cc) >> 1]));
                  const float p3 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1]));

                  grbdiffint = (p0 * grbdiff[indx >> 1]
                              + p1 * grbdiff[(indx - GRBdir[1][c]) >> 1]
                              + p2 * grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1]
                              + p3 * grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1])
                             / (p0 + p1 + p2 + p3);

                  // now determine R/B at grid points using interpolated colour differences and interpolated G
                  // value at grid point
                  if(fabsf(grbdiffold) > fabsf(grbdiffint))
                    rgb[c][indx] = rgb[1][indx] - grbdiffint;
                }

                // if colour difference interpolation overshot the correction, just desaturate
                if(grbdiffold * grbdiffint < 0)
                  rgb[c][indx] = rgb[1][indx] - 0.5f * (grbdiffold + grbdiffint);
              }
            }
            // copy CA corrected results to temporary image matrix
            for(int rr = border; rr < rr1 - border; rr++)
            {
              const int c = FC(rr + top, left + border + (FC(rr + top, 2, filters) & 1), filters);
              for(int row = rr + top, cc = border + (FC(rr, 2, filters) & 1), indx = (row * width + cc + left) >> 1;
                    cc < cc1 - border; cc += 2, indx++)
              {
                RawDataTmp[indx] = rgb[c][(rr)*ts + cc];
              }
            }
          }
        }

DT_OMP_PRAGMA(barrier)
// copy temporary image matrix back to image matrix
        DT_OMP_PRAGMA(for)
        for(int row = 0; row < height; row++)
          for(int col = 0 + (FC(row, 0, filters) & 1), indx = (row * width + col) >> 1; col < width; col += 2, indx++)
          {
            out[row * width + col] = RawDataTmp[indx];
          }
      }
      dt_free_align(data);
    }
  }

  if(avoidshift && processpasstwo)
  {
    // to avoid or at least reduce the colour shift caused by raw ca correction we compute the per pixel difference factors
    // of red and blue channel and apply a gaussian blur to them.
    // Then we apply the resulting factors per pixel on the result of raw ca correction
    DT_OMP_FOR()
    for(int row = 0; row < height; row++)
    {
      const int firstCol = FC(row, 0, filters) & 1;
      const int color    = FC(row, firstCol, filters);
      float *nongreen    = (color == 0) ? redfactor : bluefactor;
      for(int col = firstCol; col < width; col += 2)
      {
        const size_t index = (size_t)row * width + col;
        const size_t oindex = (size_t)row * h_width + col / 2;
        nongreen[(row / 2) * h_width + col / 2] = CLAMPF(oldraw[oindex] / in[index], 0.5f, 2.0f);
      }
    }

    if(height % 2)
    {
      // odd height => factors are not set in last row => use values of preceding row
      for(int col = 0; col < h_width; col++)
      {
        redfactor[(h_height-1) * h_width + col] =  redfactor[(h_height-2) * h_width + col];
        bluefactor[(h_height-1) * h_width + col] =  bluefactor[(h_height-2) * h_width + col];
      }
    }

    if(width % 2)
    {
      // odd width => factors for one channel are not set in last column => use value of preceding column
      const int ngRow = 1 - (FC(0, 0, filters) & 1);
      const int ngCol = FC(ngRow, 0, filters) & 1;
      const int color = FC(ngRow, ngCol, filters);
      float *nongreen = (color == 0) ? redfactor : bluefactor;
      for(int row = 0; row < h_height; row++)
      {
        nongreen[row * h_width + h_width - 1] = nongreen[row * h_width + h_width - 2];
      }
    }

    // blur correction factors
    float valmax[] = { 10.0f };
    float valmin[] = { 0.1f };
    dt_gaussian_t *red  = dt_gaussian_init(h_width, h_height, 1, valmax, valmin, 30.0f, 0);
    dt_gaussian_t *blue = dt_gaussian_init(h_width, h_height, 1, valmax, valmin, 30.0f, 0);
    if(red && blue)
    {
      dt_gaussian_blur(red, redfactor, redfactor);
      dt_gaussian_blur(blue, bluefactor, bluefactor);

      DT_OMP_FOR()
      for(size_t row = 2; row < height - 2; row++)
      {
        const int firstCol = FC(row, 0, filters) & 1;
        const int color = FC(row, firstCol, filters);
        float *nongreen = (color == 0) ? redfactor : bluefactor;
        for(size_t col = firstCol; col < width - 2; col += 2)
        {
          const float correction = nongreen[row / 2 * h_width + col / 2];
          out[row * width + col] *= correction;
        }
      }
    }
    if(red)  dt_gaussian_free(red);
    if(blue) dt_gaussian_free(blue);
  }

  writeout:
  DT_OMP_FOR(collapse(2))
  for(size_t row = 0; row < roi_out->height; row++)
  {
    for(size_t col = 0; col < roi_out->width; col++)
    {
      const size_t ox = row * roi_out->width + col;
      const size_t irow = row + roi_out->y;
      const size_t icol = col + roi_out->x;
      const size_t ix = irow * roi_in->width + icol;
      if((irow < roi_in->height) && (icol < roi_in->width))
      {
        output[ox] = out[ix] * scaler;
      }
    }
  }

  dt_free_align(blockwt);
  dt_free_align(out);
  dt_free_align(RawDataTmp);
  dt_free_align(Gtmp);
  dt_free_align(redfactor);
  dt_free_align(bluefactor);
  dt_free_align(oldraw);
}

/*==================================================================================
 * end raw therapee code
 *==================================================================================*/
void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  roi_out->x = MAX(0, roi_in->x);
  roi_out->y = MAX(0, roi_in->y);
}
void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
  roi_in->scale = 1.0f;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

void reload_defaults(dt_iop_module_t *self)
{
  // can't be switched on for non bayer RGB images:
  if(!dt_image_is_bayerRGB(&self->dev->image_storage))
  {
    self->hide_enable_button = TRUE;
    self->default_enabled = FALSE;
  }
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_cacorrect_params_t *p = (dt_iop_cacorrect_params_t *)params;
  dt_iop_cacorrect_data_t *d =  piece->data;

  if(!dt_image_is_bayerRGB(&self->dev->image_storage)) piece->enabled = FALSE;

  d->iterations = p->iterations;
  d->avoidshift = p->avoidshift;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = malloc(sizeof(dt_iop_cacorrect_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrect_gui_data_t *g = self->gui_data;
  dt_iop_cacorrect_params_t *p = self->params;

  const gboolean supported = dt_image_is_bayerRGB(&self->dev->image_storage);
  self->hide_enable_button = !supported;
  if(!supported) self->default_enabled = FALSE;

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), supported ? "bayer" : "other");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->avoidshift), p->avoidshift);

  gtk_widget_set_visible(g->avoidshift, supported);
  gtk_widget_set_visible(g->iterations, supported);
  dt_bauhaus_combobox_set_from_value(g->iterations, p->iterations);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_cacorrect_gui_data_t *g = IOP_GUI_ALLOC(cacorrect);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->iterations = dt_bauhaus_combobox_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations, _("iteration runs, default is twice"));

  g->avoidshift = dt_bauhaus_toggle_from_params(self, "avoidshift");
  gtk_widget_set_tooltip_text(g->avoidshift, _("activate colorshift correction for blue & red channels"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "bayer");

  GtkWidget *label_other = dt_ui_label_new(_("automatic chromatic aberration correction\nonly for Bayer raw files with 3 color channels"));
  gtk_stack_add_named(GTK_STACK(self->widget), label_other, "other");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
