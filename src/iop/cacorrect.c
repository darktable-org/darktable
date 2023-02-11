/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#pragma GCC diagnostic ignored "-Wshadow"

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
  uint32_t avoidshift;
  uint32_t iterations;
} dt_iop_cacorrect_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("raw chromatic aberrations");
}

const char **description(struct dt_iop_module_t *self)
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

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_cacorrect_params_t *n = (dt_iop_cacorrect_params_t *)new_params;
    n->avoidshift = FALSE;
    n->iterations = 1;
    return 0;
  }
  return 1;
}

/*==================================================================================
 * begin raw therapee code, hg checkout of march 09, 2016 branch master.
 *==================================================================================*/

#ifdef __GNUC__
#define INLINE __inline
#else
#define INLINE inline
#endif


static INLINE float SQR(float x)
{
  //      return std::pow(x,2); Slower than:
  return (x * x);
}
static INLINE float LIM(const float a, const float b, const float c)
{
  return MAX(b, MIN(a, c));
}
static INLINE float intp(const float a, const float b, const float c)
{
  // calculate a * b + (1 - a) * c
  // following is valid:
  // intp(a, b+x, c+x) = intp(a, b, c) + x
  // intp(a, b*x, c*x) = intp(a, b, c) * x
  return a * (b - c) + c;
}

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
static gboolean LinEqSolve(int nDim, double *pfMatr, double *pfVect, double *pfSolution)
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

  int i, j, k, m;

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

static inline void pixSort(float *a, float *b)
{
  if(*a > *b)
  {
    float temp = *a;
    *a = *b;
    *b = temp;
  }
}

/*
  We want to avoid the module being processed in case the provided size of data is too small resulting in
  really bad artifacts. This is often the case while zooming in with the current dt pipeline.
  There is no "maths background" so i chose this after a lot of testing.
*/
#define CA_SIZE_MINIMUM (1600)
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float *const in2 = (float *)i;
  float *out = (float *) o;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int h_width = (width + 1) / 2;
  const int h_height = (height + 1) / 2;

  const uint32_t filters = piece->pipe->dsc.filters;

  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  dt_iop_cacorrect_data_t     *d = (dt_iop_cacorrect_data_t *)piece->data;

  const gboolean avoidshift = d->avoidshift;
  const int iterations = d->iterations;

  // Because we can't break parallel processing, we need a switch do handle the errors
  gboolean processpasstwo = TRUE;

  float *redfactor = NULL;
  float *bluefactor = NULL;
  float *oldraw = NULL;

  dt_iop_image_copy_by_size(out, in2, width, height, 1);

  if(run_fast) return;

  const float *const in = out;

  const float caautostrength = 4.0f;

  // multithreaded and partly vectorized by Ingo Weyrich
  const int ts = 128;
  const int tsh = ts / 2;
  // shifts to location of vertical and diagonal neighbors
  const int v1 = ts, v2 = 2 * ts, v3 = 3 * ts, v4 = 4 * ts;

  // Test for RGB cfa
  for(int i = 0; i < 2; i++)
    for(int j = 0; j < 2; j++)
      if(FC(i, j, filters) == 3)
        return;

  if(avoidshift)
  {
    const size_t buffsize = (size_t)h_width * h_height;
    redfactor = dt_alloc_align_float(buffsize);
    memset(redfactor, 0, sizeof(float) * buffsize);
    bluefactor = dt_alloc_align_float(buffsize);
    memset(bluefactor, 0, sizeof(float) * buffsize);
    oldraw = dt_alloc_align_float(buffsize * 2);
    memset(oldraw, 0, sizeof(float) * buffsize * 2);
    // copy raw values before ca correction
#ifdef _OPENMP
        #pragma omp parallel for
#endif
    for(int row = 0; row < height; row++)
    {
      for(int col = (FC(row, 0, filters) & 1); col < width; col += 2)
      {
        oldraw[row * h_width + col / 2] = in[row * width + col];
      }
    }
  }

  double fitparams[2][2][16];

  // temporary array to store simple interpolation of G
  float *Gtmp = dt_alloc_align_float((size_t)height * width);
  memset(Gtmp, 0, sizeof(float) * height * width);

  // temporary array to avoid race conflicts, only every second pixel needs to be saved here
  float *RawDataTmp = dt_alloc_align_float(height * width / 2 + 4);

  const int border = 8;
  const int border2 = 16;

  const int vz1 = (height + border2) % (ts - border2) == 0 ? 1 : 0;
  const int hz1 = (width + border2) % (ts - border2) == 0 ? 1 : 0;
  const int vblsz = ceil((float)(height + border2) / (ts - border2) + 2 + vz1);
  const int hblsz = ceil((float)(width + border2) / (ts - border2) + 2 + hz1);

  char *buffer1 = (char *)calloc((size_t)vblsz * hblsz * (2 * 2 + 1), sizeof(float));

  // block CA shift values and weight assigned to block
  float *blockwt = (float *)buffer1;
  float(*blockshifts)[2][2] = (float(*)[2][2])(buffer1 + (sizeof(float) * vblsz * hblsz));

  float blockave[2][2] = { { 0, 0 }, { 0, 0 } };
  float blocksqave[2][2] = { { 0, 0 }, { 0, 0 } };
  float blockdenom[2][2] = { { 0, 0 }, { 0, 0 } };
  float blockvar[2][2];
  // order of 2d polynomial fit (polyord), and numpar=polyord^2
  int polyord = 4, numpar = 16;

  const float eps = 1e-5f, eps2 = 1e-10f; // tolerance to avoid dividing by zero

  for(size_t it = 0; it < iterations && processpasstwo; it++)
  {

#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    // direction of the CA shift in a tile
    int GRBdir[2][3];

    int shifthfloor[3], shiftvfloor[3], shifthceil[3], shiftvceil[3];

    // local quadratic fit to shift data within a tile
    float coeff[2][3][2];
    // measured CA shift parameters for a tile
    float CAshift[2][2];
    // polynomial fit coefficients
    // residual CA shift amount within a plaquette
    float shifthfrac[3], shiftvfrac[3];
    // per thread data for evaluation of block CA shift variance
    float blockavethr[2][2] = { { 0, 0 }, { 0, 0 } }, blocksqavethr[2][2] = { { 0, 0 }, { 0, 0 } },
          blockdenomthr[2][2] = { { 0, 0 }, { 0, 0 } };

    // assign working space
    const size_t buffersize = sizeof(float) * 3 * ts * ts + 6 * sizeof(float) * ts * tsh + 8 * 64 + 63;
    char *buffer = (char *)malloc(buffersize);
    char *data = (char *)(((uintptr_t)buffer + (uintptr_t)63) / 64 * 64);

    // shift the beginning of all arrays but the first by 64 bytes to avoid cache miss conflicts on CPUs which
    // have <=4-way associative L1-Cache

    // rgb data in a tile
    float *rgb[3];
    rgb[0] = (float(*))data;
    rgb[1] = (float(*))(data + 1 * sizeof(float) * ts * ts + 1 * 64);
    rgb[2] = (float(*))(data + 2 * sizeof(float) * ts * ts + 2 * 64);

    // high pass filter for R/B in vertical direction
    float *rbhpfh = (float(*))(data + 3 * sizeof(float) * ts * ts + 3 * 64);
    // high pass filter for R/B in horizontal direction
    float *rbhpfv = (float(*))(data + 3 * sizeof(float) * ts * ts + sizeof(float) * ts * tsh + 4 * 64);
    // low pass filter for R/B in horizontal direction
    float *rblpfh = (float(*))(data + 4 * sizeof(float) * ts * ts + 5 * 64);
    // low pass filter for R/B in vertical direction
    float *rblpfv = (float(*))(data + 4 * sizeof(float) * ts * ts + sizeof(float) * ts * tsh + 6 * 64);
    // low pass filter for colour differences in horizontal direction
    float *grblpfh = (float(*))(data + 5 * sizeof(float) * ts * ts + 7 * 64);
    // low pass filter for colour differences in vertical direction
    float *grblpfv = (float(*))(data + 5 * sizeof(float) * ts * ts + sizeof(float) * ts * tsh + 8 * 64);
    // colour differences
    float *grbdiff = rbhpfh; // there is no overlap in buffer usage => share
    // green interpolated to optical sample points for R/B
    float *gshift = rbhpfv; // there is no overlap in buffer usage => share

    {
// Main algorithm: Tile loop calculating correction parameters per tile
#ifdef _OPENMP
#pragma omp for collapse(2) schedule(static) nowait
#endif
      for(int top = -border; top < height; top += ts - border2)
        for(int left = -border; left < width; left += ts - border2)
        {
          memset(buffer, 0, buffersize);
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
            for(int row = rr + top, cc = ccmin; cc < ccmax; cc++)
            {
              int col = cc + left;
              int c = FC(rr, cc, filters);
              int indx = row * width + col;
              int indx1 = rr * ts + cc;
              rgb[c][indx1] = (in[indx]);
            }

          // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
          // fill borders
          if(rrmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][(border2 - rr) * ts + cc];
              }
          }

          if(rrmax < rr1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = (in[(height - rr - 2) * width + left + cc]);
              }
          }

          if(ccmin > 0)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][rr * ts + border2 - cc];
              }
          }

          if(ccmax < cc1)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + ccmax + cc] = (in[(top + rr) * width + (width - cc - 2)]);
              }
          }

          // also, fill the image corners
          if(rrmin > 0 && ccmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + cc] = (in[(border2 - rr) * width + border2 - cc]);
              }
          }

          if(rrmax < rr1 && ccmax < cc1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + ccmax + cc] = (in[(height - rr - 2) * width + (width - cc - 2)]);
              }
          }

          if(rrmin > 0 && ccmax < cc1)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + ccmax + cc] = (in[(border2 - rr) * width + (width - cc - 2)]);
              }
          }

          if(rrmax < rr1 && ccmin > 0)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = (in[(height - rr - 2) * width + (border2 - cc)]);
              }
          }

// end of border fill
// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// end of initialization

          for(int rr = 3; rr < rr1 - 3; rr++)
          {
            int row = rr + top;
            for(int cc = 3 + (FC(rr, 3, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - 3; cc += 2, indx += 2)
            {
              // compute directional weights using image gradients
              float wtu = 1.f / SQR(eps + fabsf(rgb[1][indx + v1] - rgb[1][indx - v1])
                                    + fabsf(rgb[c][indx] - rgb[c][indx - v2])
                                    + fabsf(rgb[1][indx - v1] - rgb[1][indx - v3]));
              float wtd = 1.f / SQR(eps + fabsf(rgb[1][indx - v1] - rgb[1][indx + v1])
                                    + fabsf(rgb[c][indx] - rgb[c][indx + v2])
                                    + fabsf(rgb[1][indx + v1] - rgb[1][indx + v3]));
              float wtl = 1.f / SQR(eps + fabsf(rgb[1][indx + 1] - rgb[1][indx - 1])
                                    + fabsf(rgb[c][indx] - rgb[c][indx - 2])
                                    + fabsf(rgb[1][indx - 1] - rgb[1][indx - 3]));
              float wtr = 1.f / SQR(eps + fabsf(rgb[1][indx - 1] - rgb[1][indx + 1])
                                    + fabsf(rgb[c][indx] - rgb[c][indx + 2])
                                    + fabsf(rgb[1][indx + 1] - rgb[1][indx + 3]));

              // store in rgb array the interpolated G value at R/B grid points using directional weighted
              // average
              rgb[1][indx] = (wtu * rgb[1][indx - v1] + wtd * rgb[1][indx + v1] + wtl * rgb[1][indx - 1]
                              + wtr * rgb[1][indx + 1])
                             / (wtu + wtd + wtl + wtr);
            }

            if(row > -1 && row < height)
            {
              for(int col = MAX(left + 3, 0), indx = rr * ts + 3 - (left < 0 ? (left + 3) : 0);
                  col < MIN(cc1 + left - 3, width); col++, indx++)
              {
                Gtmp[row * width + col] = rgb[1][indx];
              }
            }
          }

          for(int rr = 4; rr < rr1 - 4; rr++)
          {
            for(int cc = 4 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - 4; cc += 2, indx += 2)
            {
              rbhpfv[indx >> 1] = fabsf(
                  fabsf((rgb[1][indx] - rgb[c][indx]) - (rgb[1][indx + v4] - rgb[c][indx + v4]))
                  + fabsf((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx] - rgb[c][indx]))
                  - fabsf((rgb[1][indx - v4] - rgb[c][indx - v4]) - (rgb[1][indx + v4] - rgb[c][indx + v4])));
              rbhpfh[indx >> 1] = fabsf(
                  fabsf((rgb[1][indx] - rgb[c][indx]) - (rgb[1][indx + 4] - rgb[c][indx + 4]))
                  + fabsf((rgb[1][indx - 4] - rgb[c][indx - 4]) - (rgb[1][indx] - rgb[c][indx]))
                  - fabsf((rgb[1][indx - 4] - rgb[c][indx - 4]) - (rgb[1][indx + 4] - rgb[c][indx + 4])));

              // low and high pass 1D filters of G in vertical/horizontal directions
              float glpfv = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + v2] + rgb[1][indx - v2]);
              float glpfh = 0.25f * (2.f * rgb[1][indx] + rgb[1][indx + 2] + rgb[1][indx - 2]);
              rblpfv[indx >> 1]
                  = eps + fabsf(glpfv - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]));
              rblpfh[indx >> 1]
                  = eps + fabsf(glpfh - 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]));
              grblpfv[indx >> 1]
                  = glpfv + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + v2] + rgb[c][indx - v2]);
              grblpfh[indx >> 1] = glpfh + 0.25f * (2.f * rgb[c][indx] + rgb[c][indx + 2] + rgb[c][indx - 2]);
            }
          }

          for(int dir = 0; dir < 2; dir++)
          {
            for(int k = 0; k < 3; k++)
            {
              for(int c = 0; c < 2; c++)
              {
                coeff[dir][k][c] = 0;
              }
            }
          }

          // along line segments, find the point along each segment that minimizes the colour variance
          // averaged over the tile; evaluate for up/down and left/right away from R/B grid point
          for(int rr = 8; rr < rr1 - 8; rr++)
          {
            for(int cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = FC(rr, cc, filters); cc < cc1 - 8; cc += 2, indx += 2)
            {

              // in linear interpolation, colour differences are a quadratic function of interpolation
              // position;
              // solve for the interpolation position that minimizes colour difference variance over the tile

              // vertical
              float gdiff = 0.3125f * (rgb[1][indx + ts] - rgb[1][indx - ts])
                            + 0.09375f * (rgb[1][indx + ts + 1] - rgb[1][indx - ts + 1]
                                          + rgb[1][indx + ts - 1] - rgb[1][indx - ts - 1]);
              float deltgrb = (rgb[c][indx] - rgb[1][indx]);

              float gradwt = fabsf(0.25f * rbhpfv[indx >> 1]
                                   + 0.125f * (rbhpfv[(indx >> 1) + 1] + rbhpfv[(indx >> 1) - 1]))
                             * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1])
                             / (eps + 0.1f * (grblpfv[(indx >> 1) - v1] + grblpfv[(indx >> 1) + v1])
                                + rblpfv[(indx >> 1) - v1] + rblpfv[(indx >> 1) + v1]);

              coeff[0][0][c >> 1] += gradwt * deltgrb * deltgrb;
              coeff[0][1][c >> 1] += gradwt * gdiff * deltgrb;
              coeff[0][2][c >> 1] += gradwt * gdiff * gdiff;

              // horizontal
              gdiff = 0.3125f * (rgb[1][indx + 1] - rgb[1][indx - 1])
                      + 0.09375f * (rgb[1][indx + 1 + ts] - rgb[1][indx - 1 + ts] + rgb[1][indx + 1 - ts]
                                    - rgb[1][indx - 1 - ts]);

              gradwt = fabsf(0.25f * rbhpfh[indx >> 1]
                             + 0.125f * (rbhpfh[(indx >> 1) + v1] + rbhpfh[(indx >> 1) - v1]))
                       * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1])
                       / (eps + 0.1f * (grblpfh[(indx >> 1) - 1] + grblpfh[(indx >> 1) + 1])
                          + rblpfh[(indx >> 1) - 1] + rblpfh[(indx >> 1) + 1]);

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
                blockwt[vblock * hblsz + hblock] = coeff[dir][2][c] / (eps + coeff[dir][0][c]);
              }
              else
              {
                CAshift[dir][c] = 17.0;
                blockwt[vblock * hblsz + hblock] = 0;
              }

              // data structure = CAshift[vert/hor][colour]
              // dir : 0=vert, 1=hor

              // offset gives NW corner of square containing the min; dir : 0=vert, 1=hor
              if(fabsf(CAshift[dir][c]) < 2.0f)
              {
                blockavethr[dir][c] += CAshift[dir][c];
                blocksqavethr[dir][c] += SQR(CAshift[dir][c]);
                blockdenomthr[dir][c] += 1;
              }
              // evaluate the shifts to the location that minimizes CA within the tile
              blockshifts[vblock * hblsz + hblock][c][dir] = CAshift[dir][c]; // vert/hor CA shift for R/B

            } // vert/hor
          }   // colour

        }

// end of diagnostic pass
#ifdef _OPENMP
#pragma omp critical(cadetectpass2)
#endif
      {
        for(int dir = 0; dir < 2; dir++)
          for(int c = 0; c < 2; c++)
          {
            blockdenom[dir][c] += blockdenomthr[dir][c];
            blocksqave[dir][c] += blocksqavethr[dir][c];
            blockave[dir][c] += blockavethr[dir][c];
          }
      }
#ifdef _OPENMP
#pragma omp barrier
#endif

#ifdef _OPENMP
#pragma omp single
#endif
      {
        for(int dir = 0; dir < 2; dir++)
          for(int c = 0; c < 2; c++)
          {
            if(blockdenom[dir][c])
            {
              blockvar[dir][c]
                  = blocksqave[dir][c] / blockdenom[dir][c] - SQR(blockave[dir][c] / blockdenom[dir][c]);
            }
            else
            {
              processpasstwo = FALSE;
              dt_print(DT_DEBUG_PIPE, "[cacorrect] blockdenom vanishes\n");
              break;
            }
          }

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // now prepare for CA correction pass
        // first, fill border blocks of blockshift array
        if(processpasstwo)
        {
          for(int vblock = 1; vblock < vblsz - 1; vblock++)
          { // left and right sides
            for(int c = 0; c < 2; c++)
            {
              for(int i = 0; i < 2; i++)
              {
                blockshifts[vblock * hblsz][c][i] = blockshifts[(vblock)*hblsz + 2][c][i];
                blockshifts[vblock * hblsz + hblsz - 1][c][i] = blockshifts[(vblock)*hblsz + hblsz - 3][c][i];
              }
            }
          }

          for(int hblock = 0; hblock < hblsz; hblock++)
          { // top and bottom sides
            for(int c = 0; c < 2; c++)
            {
              for(int i = 0; i < 2; i++)
              {
                blockshifts[hblock][c][i] = blockshifts[2 * hblsz + hblock][c][i];
                blockshifts[(vblsz - 1) * hblsz + hblock][c][i]
                    = blockshifts[(vblsz - 3) * hblsz + hblock][c][i];
              }
            }
          }

          // end of filling border pixels of blockshift array

          // initialize fit arrays
          double polymat[2][2][256], shiftmat[2][2][16];

          for(int i = 0; i < 256; i++)
          {
            polymat[0][0][i] = polymat[0][1][i] = polymat[1][0][i] = polymat[1][1][i] = 0;
          }

          for(int i = 0; i < 16; i++)
          {
            shiftmat[0][0][i] = shiftmat[0][1][i] = shiftmat[1][0][i] = shiftmat[1][1][i] = 0;
          }

          int numblox[2] = { 0, 0 };

          for(int vblock = 1; vblock < vblsz - 1; vblock++)
            for(int hblock = 1; hblock < hblsz - 1; hblock++)
            {
              // block 3x3 median of blockshifts for robustness
              for(int c = 0; c < 2; c++)
              {
                float bstemp[2];
                for(int dir = 0; dir < 2; dir++)
                {
                  // temporary storage for median filter
                  float p[9];
                  p[0] = blockshifts[(vblock - 1) * hblsz + hblock - 1][c][dir];
                  p[1] = blockshifts[(vblock - 1) * hblsz + hblock][c][dir];
                  p[2] = blockshifts[(vblock - 1) * hblsz + hblock + 1][c][dir];
                  p[3] = blockshifts[(vblock)*hblsz + hblock - 1][c][dir];
                  p[4] = blockshifts[(vblock)*hblsz + hblock][c][dir];
                  p[5] = blockshifts[(vblock)*hblsz + hblock + 1][c][dir];
                  p[6] = blockshifts[(vblock + 1) * hblsz + hblock - 1][c][dir];
                  p[7] = blockshifts[(vblock + 1) * hblsz + hblock][c][dir];
                  p[8] = blockshifts[(vblock + 1) * hblsz + hblock + 1][c][dir];
                  pixSort(&p[1], &p[2]);
                  pixSort(&p[4], &p[5]);
                  pixSort(&p[7], &p[8]);
                  pixSort(&p[0], &p[1]);
                  pixSort(&p[3], &p[4]);
                  pixSort(&p[6], &p[7]);
                  pixSort(&p[1], &p[2]);
                  pixSort(&p[4], &p[5]);
                  pixSort(&p[7], &p[8]);
                  pixSort(&p[0], &p[3]);
                  pixSort(&p[5], &p[8]);
                  pixSort(&p[4], &p[7]);
                  pixSort(&p[3], &p[6]);
                  pixSort(&p[1], &p[4]);
                  pixSort(&p[2], &p[5]);
                  pixSort(&p[4], &p[7]);
                  pixSort(&p[4], &p[2]);
                  pixSort(&p[6], &p[4]);
                  pixSort(&p[4], &p[2]);
                  bstemp[dir] = p[4];
                }

                // now prepare coefficient matrix; use only data points within caautostrength/2 std devs of
                // zero
                if(SQR(bstemp[0]) > caautostrength * blockvar[0][c]
                   || SQR(bstemp[1]) > caautostrength * blockvar[1][c])
                {
                  continue;
                }

                numblox[c]++;

                for(int dir = 0; dir < 2; dir++)
                {
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
                          polymat[c][dir][numpar * (polyord * i + j) + (polyord * m + n)]
                              += powVblock * powHblock * blockwt[vblock * hblsz + hblock];
                          powHblock *= hblock;
                        }
                        powVblock *= vblock;
                      }
                      shiftmat[c][dir][(polyord * i + j)]
                          += powVblockInit * powHblockInit * bstemp[dir] * blockwt[vblock * hblsz + hblock];
                      powHblockInit *= hblock;
                    }
                    powVblockInit *= vblock;
                  } // monomials
                }   // dir
              }     // c
            }       // blocks

          numblox[1] = MIN(numblox[0], numblox[1]);

          // if too few data points, restrict the order of the fit to linear
          if(numblox[1] < 32)
          {
            polyord = 2;
            numpar = 4;

            if(numblox[1] < 10)
            {
              dt_print(DT_DEBUG_PIPE, "[cacorrect] restrict fit to linear, numblox = %d \n", numblox[1]);
              processpasstwo = FALSE;
            }
          }

          if(processpasstwo)

            // fit parameters to blockshifts
            for(int c = 0; c < 2; c++)
              for(int dir = 0; dir < 2; dir++)
              {
                if(!LinEqSolve(numpar, polymat[c][dir], shiftmat[c][dir], fitparams[c][dir]))
                {
                  dt_print(DT_DEBUG_PIPE, "[cacorrect] can't solve linear equations for colour %d direction %d", c, dir);
                  processpasstwo = FALSE;
                }
              }
        }

        // fitparams[polyord*i+j] gives the coefficients of (vblock^i hblock^j) in a polynomial fit for i,j<=4
      }
      // end of initialization for CA correction pass
      // only executed if cared and cablue are zero
    }

    // Main algorithm: Tile loop
    if(processpasstwo)
    {
#ifdef _OPENMP
#pragma omp for schedule(static) collapse(2) nowait
#endif

      for(int top = -border; top < height; top += ts - border2)
        for(int left = -border; left < width; left += ts - border2)
        {
          memset(buffer, 0, buffersize);
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
            for(int row = rr + top, cc = ccmin; cc < ccmax; cc++)
            {
              int col = cc + left;
              int c = FC(rr, cc, filters);
              int indx = row * width + col;
              int indx1 = rr * ts + cc;
              rgb[c][indx1] = (in[indx]);

              if((c & 1) == 0)
              {
                rgb[1][indx1] = Gtmp[indx];
              }
            }

          // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
          // fill borders
          if(rrmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][(border2 - rr) * ts + cc];
                rgb[1][rr * ts + cc] = rgb[1][(border2 - rr) * ts + cc];
              }
          }

          if(rrmax < rr1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = ccmin; cc < ccmax; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = (in[(height - rr - 2) * width + left + cc]);
                rgb[1][(rrmax + rr) * ts + cc] = Gtmp[(height - rr - 2) * width + left + cc];
              }
          }

          if(ccmin > 0)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + cc] = rgb[c][rr * ts + border2 - cc];
                rgb[1][rr * ts + cc] = rgb[1][rr * ts + border2 - cc];
              }
          }

          if(ccmax < cc1)
          {
            for(int rr = rrmin; rr < rrmax; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][rr * ts + ccmax + cc] = (in[(top + rr) * width + (width - cc - 2)]);
                rgb[1][rr * ts + ccmax + cc] = Gtmp[(top + rr) * width + (width - cc - 2)];
              }
          }

          // also, fill the image corners
          if(rrmin > 0 && ccmin > 0)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + cc] = (in[(border2 - rr) * width + border2 - cc]);
                rgb[1][(rr)*ts + cc] = Gtmp[(border2 - rr) * width + border2 - cc];
              }
          }

          if(rrmax < rr1 && ccmax < cc1)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + ccmax + cc] = (in[(height - rr - 2) * width + (width - cc - 2)]);
                rgb[1][(rrmax + rr) * ts + ccmax + cc] = Gtmp[(height - rr - 2) * width + (width - cc - 2)];
              }
          }

          if(rrmin > 0 && ccmax < cc1)
          {
            for(int rr = 0; rr < border; rr++)
              for(int cc = 0; cc < MIN(border, cc1 - ccmax); cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rr)*ts + ccmax + cc] = (in[(border2 - rr) * width + (width - cc - 2)]);
                rgb[1][(rr)*ts + ccmax + cc] = Gtmp[(border2 - rr) * width + (width - cc - 2)];
              }
          }

          if(rrmax < rr1 && ccmin > 0)
          {
            for(int rr = 0; rr < MIN(border, rr1 - rrmax); rr++)
              for(int cc = 0; cc < border; cc++)
              {
                int c = FC(rr, cc, filters);
                rgb[c][(rrmax + rr) * ts + cc] = (in[(height - rr - 2) * width + (border2 - cc)]);
                rgb[1][(rrmax + rr) * ts + cc] = Gtmp[(height - rr - 2) * width + (border2 - cc)];
              }
          }

          // end of border fill
          {
            // CA auto correction; use CA diagnostic pass to set shift parameters
            lblockshifts[0][0] = lblockshifts[0][1] = 0;
            lblockshifts[1][0] = lblockshifts[1][1] = 0;
            double powVblock = 1.0;
            for(int i = 0; i < polyord; i++)
            {
              double powHblock = powVblock;
              for(int j = 0; j < polyord; j++)
              {
                // printf("i= %d j= %d polycoeff= %f \n",i,j,fitparams[0][0][polyord*i+j]);
                lblockshifts[0][0] += powHblock * fitparams[0][0][polyord * i + j];
                lblockshifts[0][1] += powHblock * fitparams[0][1][polyord * i + j];
                lblockshifts[1][0] += powHblock * fitparams[1][0][polyord * i + j];
                lblockshifts[1][1] += powHblock * fitparams[1][1][polyord * i + j];
                powHblock *= hblock;
              }
              powVblock *= vblock;
            }
            const float bslim = 3.99; // max allowed CA shift
            lblockshifts[0][0] = LIM(lblockshifts[0][0], -bslim, bslim);
            lblockshifts[0][1] = LIM(lblockshifts[0][1], -bslim, bslim);
            lblockshifts[1][0] = LIM(lblockshifts[1][0], -bslim, bslim);
            lblockshifts[1][1] = LIM(lblockshifts[1][1], -bslim, bslim);
          } // end of setting CA shift parameters


          for(int c = 0; c < 3; c += 2)
          {

            // some parameters for the bilinear interpolation
            shiftvfloor[c] = floor((float)lblockshifts[c >> 1][0]);
            shiftvceil[c] = ceil((float)lblockshifts[c >> 1][0]);
            if(lblockshifts[c>>1][0] < 0.f) {
              float tmp = shiftvfloor[c];
              shiftvfloor[c] = shiftvceil[c];
              shiftvceil[c] = tmp;
            }
            shiftvfrac[c] = fabsf(lblockshifts[c>>1][0] - shiftvfloor[c]);

            shifthfloor[c] = floor((float)lblockshifts[c >> 1][1]);
            shifthceil[c] = ceil((float)lblockshifts[c >> 1][1]);
            if(lblockshifts[c>>1][1] < 0.f) {
              float tmp = shifthfloor[c];
              shifthfloor[c] = shifthceil[c];
              shifthceil[c] = tmp;
            }
            shifthfrac[c] = fabsf(lblockshifts[c>>1][1] - shifthfloor[c]);


            GRBdir[0][c] = lblockshifts[c >> 1][0] > 0 ? 2 : -2;
            GRBdir[1][c] = lblockshifts[c >> 1][1] > 0 ? 2 : -2;
          }


          for(int rr = 4; rr < rr1 - 4; rr++)
          {
            for(int cc = 4 + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters); cc < cc1 - 4; cc += 2)
            {
              // perform CA correction using colour ratios or colour differences
              float Ginthfloor = intp(shifthfrac[c], rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthceil[c]],
                                      rgb[1][(rr + shiftvfloor[c]) * ts + cc + shifthfloor[c]]);
              float Ginthceil = intp(shifthfrac[c], rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthceil[c]],
                                     rgb[1][(rr + shiftvceil[c]) * ts + cc + shifthfloor[c]]);
              // Gint is bilinear interpolation of G at CA shift point
              float Gint = intp(shiftvfrac[c], Ginthceil, Ginthfloor);

              // determine R/B at grid points using colour differences at shift point plus interpolated G
              // value at grid point
              // but first we need to interpolate G-R/G-B to grid points...
              grbdiff[((rr)*ts + cc) >> 1] = Gint - rgb[c][(rr)*ts + cc];
              gshift[((rr)*ts + cc) >> 1] = Gint;
            }
          }

          shifthfrac[0] /= 2.f;
          shifthfrac[2] /= 2.f;
          shiftvfrac[0] /= 2.f;
          shiftvfrac[2] /= 2.f;

          // this loop does not deserve vectorization in mainly because the most expensive part with the
          // divisions does not happen often (less than 1/10 in my tests)
          for(int rr = 8; rr < rr1 - 8; rr++)
            for(int cc = 8 + (FC(rr, 2, filters) & 1), c = FC(rr, cc, filters), indx = rr * ts + cc;
                cc < cc1 - 8; cc += 2, indx += 2)
            {

              float grbdiffold = rgb[1][indx] - rgb[c][indx];

              // interpolate colour difference from optical R/B locations to grid locations
              float grbdiffinthfloor
                  = intp(shifthfrac[c], grbdiff[(indx - GRBdir[1][c]) >> 1], grbdiff[indx >> 1]);
              float grbdiffinthceil
                  = intp(shifthfrac[c], grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1],
                         grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1]);
              // grbdiffint is bilinear interpolation of G-R/G-B at grid point
              float grbdiffint = intp(shiftvfrac[c], grbdiffinthceil, grbdiffinthfloor);

              // now determine R/B at grid points using interpolated colour differences and interpolated G
              // value at grid point
              float RBint = rgb[1][indx] - grbdiffint;

              if(fabsf(RBint - rgb[c][indx]) < 0.25f * (RBint + rgb[c][indx]))
              {
                if(fabsf(grbdiffold) > fabsf(grbdiffint))
                {
                  rgb[c][indx] = RBint;
                }
              }
              else
              {

                // gradient weights using difference from G at CA shift points and G at grid points
                float p0 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[indx >> 1]));
                float p1 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[(indx - GRBdir[1][c]) >> 1]));
                float p2 = 1.0f / (eps + fabsf(rgb[1][indx] - gshift[((rr - GRBdir[0][c]) * ts + cc) >> 1]));
                float p3
                    = 1.0f / (eps + fabsf(rgb[1][indx]
                                          - gshift[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1]));

                grbdiffint = (p0 * grbdiff[indx >> 1] + p1 * grbdiff[(indx - GRBdir[1][c]) >> 1]
                              + p2 * grbdiff[((rr - GRBdir[0][c]) * ts + cc) >> 1]
                              + p3 * grbdiff[((rr - GRBdir[0][c]) * ts + cc - GRBdir[1][c]) >> 1])
                             / (p0 + p1 + p2 + p3);

                // now determine R/B at grid points using interpolated colour differences and interpolated G
                // value at grid point
                if(fabsf(grbdiffold) > fabsf(grbdiffint))
                {
                  rgb[c][indx] = rgb[1][indx] - grbdiffint;
                }
              }

              // if colour difference interpolation overshot the correction, just desaturate
              if(grbdiffold * grbdiffint < 0)
              {
                rgb[c][indx] = rgb[1][indx] - 0.5f * (grbdiffold + grbdiffint);
              }
            }

          // copy CA corrected results to temporary image matrix
          for(int rr = border; rr < rr1 - border; rr++)
          {
            int c = FC(rr + top, left + border + (FC(rr + top, 2, filters) & 1), filters);

            for(int row = rr + top, cc = border + (FC(rr, 2, filters) & 1),
                    indx = (row * width + cc + left) >> 1;
                cc < cc1 - border; cc += 2, indx++)
            {
              //               int col = cc + left;
              RawDataTmp[indx] = rgb[c][(rr)*ts + cc];
            }
          }
        }

#ifdef _OPENMP
#pragma omp barrier
#endif
// copy temporary image matrix back to image matrix
#ifdef _OPENMP
#pragma omp for
#endif

      for(int row = 0; row < height; row++)
        for(int col = 0 + (FC(row, 0, filters) & 1), indx = (row * width + col) >> 1; col < width;
            col += 2, indx++)
        {
          out[row * width + col] = RawDataTmp[indx];
        }
    }

    // clean up
    free(buffer);
  }
  }

  if(avoidshift && processpasstwo)
  {
    // to avoid or at least reduce the colour shift caused by raw ca correction we compute the per pixel difference factors
    // of red and blue channel and apply a gaussian blur to them.
    // Then we apply the resulting factors per pixel on the result of raw ca correction
#ifdef _OPENMP
  #pragma omp parallel for
#endif
    for(int row = 0; row < height; row++)
    {
      const int firstCol = FC(row, 0, filters) & 1;
      const int color    = FC(row, firstCol, filters);
      float *nongreen    = (color == 0) ? redfactor : bluefactor;
      for(int col = firstCol; col < width; col += 2)
      {
        nongreen[(row / 2) * h_width + col / 2] = (in[row * width + col] <= 1.0f || oldraw[row * h_width + col / 2] <= 1.0f)
          ? 1.0f : LIM(oldraw[row * h_width + col / 2] / in[row * width + col], 0.5f, 2.0f);
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

#ifdef _OPENMP
  #pragma omp for
#endif
      for(int row = 2; row < height - 2; row++)
      {
        const int firstCol = FC(row, 0, filters) & 1;
        const int color = FC(row, firstCol, filters);
        float *nongreen = (color == 0) ? redfactor : bluefactor;
        for(int col = firstCol; col < width - 2; col += 2)
        {
          const float correction = nongreen[row / 2 * h_width + col / 2];
          out[row * width + col] *= correction;
        }
      }
    }
    if(red)  dt_gaussian_free(red);
    if(blue) dt_gaussian_free(blue);
  }

  free(buffer1);
  dt_free_align(RawDataTmp);
  dt_free_align(Gtmp);
  dt_free_align(redfactor);
  dt_free_align(bluefactor);
  dt_free_align(oldraw);
}

/*==================================================================================
 * end raw therapee code
 *==================================================================================*/

void reload_defaults(dt_iop_module_t *module)
{
  dt_image_t *img = &module->dev->image_storage;
  // can't be switched on for non-raw or x-trans images:
  const gboolean active = (dt_image_is_raw(img) && (img->buf_dsc.filters != 9u) && !(dt_image_is_monochrome(img)));
  module->hide_enable_button = !active;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_cacorrect_params_t *p = (dt_iop_cacorrect_params_t *)params;
  dt_iop_cacorrect_data_t *d = (dt_iop_cacorrect_data_t *) piece->data;

  dt_image_t *img = &pipe->image;
  const gboolean active = (dt_image_is_raw(img) && (img->buf_dsc.filters != 9u) && !(dt_image_is_monochrome(img)));

  if(!active) piece->enabled = 0;

  d->iterations = p->iterations;
  d->avoidshift = p->avoidshift;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = malloc(sizeof(dt_iop_cacorrect_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrect_gui_data_t *g = (dt_iop_cacorrect_gui_data_t *)self->gui_data;
  dt_iop_cacorrect_params_t *p = (dt_iop_cacorrect_params_t *)self->params;

  dt_image_t *img = &self->dev->image_storage;

  const gboolean active = (dt_image_is_raw(img) && (img->buf_dsc.filters != 9u) && !(dt_image_is_monochrome(img)));
  self->hide_enable_button = !active;

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), active ? "raw" : "non_raw");

  gtk_widget_set_visible(g->avoidshift, active);
  gtk_widget_set_visible(g->iterations, active);
  dt_bauhaus_combobox_set_from_value(g->iterations, p->iterations);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->avoidshift), p->avoidshift);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_cacorrect_gui_data_t *g = (dt_iop_cacorrect_gui_data_t *)self->gui_data;
  dt_iop_cacorrect_params_t *p = (dt_iop_cacorrect_params_t *)self->params;

  dt_image_t *img = &self->dev->image_storage;

  const gboolean active = (dt_image_is_raw(img) && (img->buf_dsc.filters != 9u) && !(dt_image_is_monochrome(img)));

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), active ? "raw" : "non_raw");

  gtk_widget_set_visible(g->avoidshift, active);
  dt_bauhaus_combobox_set_from_value(g->iterations, p->iterations);
  gtk_widget_set_visible(g->iterations, active);
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
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");

  GtkWidget *label_non_raw = dt_ui_label_new(_("automatic chromatic aberration correction\nonly for Bayer raw files"));
  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
}

#undef CA_SIZE_MINIMUM
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

