/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

static __inline float clampnan(const float x, const float m, const float M)
{
  float r;

  // clamp to [m, M] if x is infinite; return average of m and M if x is NaN; else just return x

  if(isinf(x))
    r = (isless(x, m) ? m : (isgreater(x, M) ? M : x));
  else if(isnan(x))
    r = (m + M) / 2.0f;
  else // normal number
    r = x;

  return r;
}

static __inline float xmul2f(float d)
{
  if(*(int *)&d & 0x7FFFFFFF) // if f==0 do nothing
  {
    *(int *)&d += 1 << 23; // add 1 to the exponent
  }
  return d;
}

static __inline float xdiv2f(float d)
{
  if(*(int *)&d & 0x7FFFFFFF) // if f==0 do nothing
  {
    *(int *)&d -= 1 << 23; // sub 1 from the exponent
  }
  return d;
}

static __inline float xdivf(float d, int n)
{
  if(*(int *)&d & 0x7FFFFFFF) // if f==0 do nothing
  {
    *(int *)&d -= n << 23; // add n to the exponent
  }
  return d;
}

/*==================================================================================
 * begin raw therapee code, hg checkout of march 23, 2014 branch master.
 *==================================================================================*/

#ifdef __SSE2__
#define ZEROV _mm_setzero_ps()
#define LVF(x) _mm_load_ps(&x)
#define LVFU(x) _mm_loadu_ps(&x)
#define LC2VFU(a) _mm_shuffle_ps(LVFU(a), _mm_loadu_ps((&a) + 4), _MM_SHUFFLE(2, 0, 2, 0))
typedef __m128i vmask;
typedef __m128 vfloat;
static __inline vfloat vcast_vf_f(float f)
{
  return _mm_set_ps(f, f, f, f);
}
static __inline vmask vorm(vmask x, vmask y)
{
  return _mm_or_si128(x, y);
}
static __inline vmask vandm(vmask x, vmask y)
{
  return _mm_and_si128(x, y);
}
static __inline vmask vandnotm(vmask x, vmask y)
{
  return _mm_andnot_si128(x, y);
}
static __inline vfloat vabsf(vfloat f)
{
  return (vfloat)vandnotm((vmask)vcast_vf_f(-0.0f), (vmask)f);
}
static __inline vfloat vself(vmask mask, vfloat x, vfloat y)
{
  return (vfloat)vorm(vandm(mask, (vmask)x), vandnotm(mask, (vmask)y));
}
static __inline vmask vmaskf_lt(vfloat x, vfloat y)
{
  return (__m128i)_mm_cmplt_ps(x, y);
}
static __inline vmask vmaskf_gt(vfloat x, vfloat y)
{
  return (__m128i)_mm_cmpgt_ps(x, y);
}
static __inline vfloat LIMV(vfloat a, vfloat b, vfloat c)
{
  return _mm_max_ps(b, _mm_min_ps(a, c));
}
static __inline vfloat ULIMV(vfloat a, vfloat b, vfloat c)
{
  return vself(vmaskf_lt(b, c), LIMV(a, b, c), LIMV(a, c, b));
}
static __inline vfloat SQRV(vfloat a)
{
  return _mm_mul_ps(a, a);
}

#endif

////////////////////////////////////////////////////////////////
//
//			AMaZE demosaic algorithm
// (Aliasing Minimization and Zipper Elimination)
//
//	copyright (c) 2008-2010  Emil Martinec <ejmartin@uchicago.edu>
//
// incorporating ideas of Luis Sanz Rodrigues and Paul Lee
//
// code dated: May 27, 2010
//
//	amaze_interpolate_RT.cc is free software: you can redistribute it and/or modify
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
//	along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////

// namespace rtengine {

// SSEFUNCTION void RawImageSource::amaze_demosaic_RT(int winx, int winy, int winw, int winh) {
static void amaze_demosaic_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                              const float *const in, float *out, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int filters)
{
#define SQR(x) ((x) * (x))
//#define MIN(a,b) ((a) < (b) ? (a) : (b))
//#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x, min, max) MAX(min, MIN(x, max))
#define ULIM(x, y, z) ((y) < (z) ? LIM(x, y, z) : LIM(x, z, y))
//#define CLIP(x) LIM(x,0,65535)
#define HCLIP(x) x // is this still necessary???
  // MIN(clip_pt,x)

  int winx = roi_out->x;
  int winy = roi_out->y;
  int winw = roi_in->width;
  int winh = roi_in->height;
  int width = winw, height = winh;


  // 	const float clip_pt = 1/initialGain;
  // 	const float clip_pt8 = 0.8f/initialGain;
  const float clip_pt = fminf(piece->pipe->processed_maximum[0],
                              fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  const float clip_pt8 = 0.8f * clip_pt;

#define TS                                                                                                   \
  160 // Tile size; the image is processed in square tiles to lower memory requirements and facilitate
      // multi-threading
#define TSH 80 // half of Tile size

  // local variables


  // offset of R pixel within a Bayer quartet
  int ex, ey;

  // shifts of pointer value to access pixels in vertical and diagonal directions
  static const int v1 = TS, v2 = 2 * TS, v3 = 3 * TS, p1 = -TS + 1, p2 = -2 * TS + 2, p3 = -3 * TS + 3,
                   m1 = TS + 1, m2 = 2 * TS + 2, m3 = 3 * TS + 3;

  // tolerance to avoid dividing by zero
  static const float eps = 1e-5, epssq = 1e-10; // tolerance to avoid dividing by zero

  // adaptive ratios threshold
  static const float arthresh = 0.75;
  // nyquist texture test threshold
  static const float nyqthresh = 0.5;

  // gaussian on 5x5 quincunx, sigma=1.2
  static const float gaussodd[4]
      = { 0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f, 0.0365543548389495f };
  // gaussian on 5x5, sigma=1.2
  static const float gaussgrad[6] = { 0.07384411893421103f, 0.06207511968171489f, 0.0521818194747806f,
                                      0.03687419286733595f, 0.03099732204057846f, 0.018413194161458882f };
  // gaussian on 5x5 alt quincunx, sigma=1.5
  static const float gausseven[2] = { 0.13719494435797422f, 0.05640252782101291f };
  // guassian on quincunx grid
  static const float gquinc[4] = { 0.169917f, 0.108947f, 0.069855f, 0.0287182f };

  // volatile double progress = 0.0;

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

  // Issue 1676
  // Moved from inside the parallel section
  // if (plistener)
  // {
  //  plistener->setProgressStr (Glib::ustring::compose(M("TP_RAW_DMETHOD_PROGRESSBAR"),
  //  RAWParams::methodstring[RAWParams::amaze]));
  //  plistener->setProgress (0.0);
  // }
  typedef struct s_hv
  {
    float h;
    float v;
  } s_hv;

#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    // int progresscounter=0;
    // position of top/left corner of the tile
    int top, left;
    // beginning of storage block for tile
    char *buffer;
    // green values
    float(*rgbgreen);

    // sum of square of horizontal gradient and square of vertical gradient
    float(*delhvsqsum);
    // gradient based directional weights for interpolation
    float(*dirwts0);
    float(*dirwts1);

    // vertically interpolated color differences G-R, G-B
    float(*vcd);
    // horizontally interpolated color differences
    float(*hcd);
    // alternative vertical interpolation
    float(*vcdalt);
    // alternative horizontal interpolation
    float(*hcdalt);
    // square of average color difference
    float(*cddiffsq);
    // weight to give horizontal vs vertical interpolation
    float(*hvwt);
    // final interpolated color difference
    float(*Dgrb)[TS * TSH];
    //	float (*Dgrb)[2];
    // gradient in plus (NE/SW) direction
    float(*delp);
    // gradient in minus (NW/SE) direction
    float(*delm);
    // diagonal interpolation of R+B
    float(*rbint);
    // horizontal and vertical curvature of interpolated G (used to refine interpolation in Nyquist texture
    // regions)
    s_hv(*Dgrb2);
    // difference between up/down interpolations of G
    float(*dgintv);
    // difference between left/right interpolations of G
    float(*dginth);
    // diagonal (plus) color difference R-B or G1-G2
    //	float (*Dgrbp1);
    // diagonal (minus) color difference R-B or G1-G2
    //	float (*Dgrbm1);
    float(*Dgrbsq1m);
    float(*Dgrbsq1p);
    //	s_mp  (*Dgrbsq1);
    // square of diagonal color difference
    //	float (*Dgrbpsq1);
    // square of diagonal color difference
    //	float (*Dgrbmsq1);
    // tile raw data
    float(*cfa);
    // relative weight for combining plus and minus diagonal interpolations
    float(*pmwt);
    // interpolated color difference R-B in minus and plus direction
    float(*rbm);
    float(*rbp);

    // nyquist texture flag 1=nyquist, 0=not nyquist
    char(*nyquist);

#define CLF 1
    // assign working space
    buffer = (char *)calloc(22 * sizeof(float) * TS * TS + sizeof(char) * TS * TSH + 23 * CLF * 64 + 63, 1);
    char *data;
    data = (char *)(((uintptr_t)buffer + (uintptr_t)63) / 64 * 64);

    // merror(buffer,"amaze_interpolate()");
    rgbgreen = (float(*))data; // pointers to array
    delhvsqsum = (float(*))((char *)rgbgreen + sizeof(float) * TS * TS + CLF * 64);
    dirwts0 = (float(*))((char *)delhvsqsum + sizeof(float) * TS * TS + CLF * 64);
    dirwts1 = (float(*))((char *)dirwts0 + sizeof(float) * TS * TS + CLF * 64);
    vcd = (float(*))((char *)dirwts1 + sizeof(float) * TS * TS + CLF * 64);
    hcd = (float(*))((char *)vcd + sizeof(float) * TS * TS + CLF * 64);
    vcdalt = (float(*))((char *)hcd + sizeof(float) * TS * TS + CLF * 64);
    hcdalt = (float(*))((char *)vcdalt + sizeof(float) * TS * TS + CLF * 64);
    cddiffsq = (float(*))((char *)hcdalt + sizeof(float) * TS * TS + CLF * 64);
    hvwt = (float(*))((char *)cddiffsq + sizeof(float) * TS * TS + CLF * 64);
    Dgrb = (float(*)[TS * TSH])((char *)hvwt + sizeof(float) * TS * TSH + CLF * 64);
    delp = (float(*))((char *)Dgrb + sizeof(float) * TS * TS + CLF * 64);
    delm = (float(*))((char *)delp + sizeof(float) * TS * TSH + CLF * 64);
    rbint = (float(*))((char *)delm + sizeof(float) * TS * TSH + CLF * 64);
    Dgrb2 = (s_hv(*))((char *)rbint + sizeof(float) * TS * TSH + CLF * 64);
    dgintv = (float(*))((char *)Dgrb2 + sizeof(float) * TS * TS + CLF * 64);
    dginth = (float(*))((char *)dgintv + sizeof(float) * TS * TS + CLF * 64);
    Dgrbsq1m = (float(*))((char *)dginth + sizeof(float) * TS * TS + CLF * 64);
    Dgrbsq1p = (float(*))((char *)Dgrbsq1m + sizeof(float) * TS * TSH + CLF * 64);
    cfa = (float(*))((char *)Dgrbsq1p + sizeof(float) * TS * TSH + CLF * 64);
    pmwt = (float(*))((char *)cfa + sizeof(float) * TS * TS + CLF * 64);
    rbm = (float(*))((char *)pmwt + sizeof(float) * TS * TSH + CLF * 64);
    rbp = (float(*))((char *)rbm + sizeof(float) * TS * TSH + CLF * 64);

    nyquist = (char(*))((char *)rbp + sizeof(float) * TS * TSH + CLF * 64);
#undef CLF
    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


    // determine GRBG coset; (ey,ex) is the offset of the R subarray
    if(FC(0, 0, filters) == 1) // first pixel is G
    {
      if(FC(0, 1, filters) == 0)
      {
        ey = 0;
        ex = 1;
      }
      else
      {
        ey = 1;
        ex = 0;
      }
    }
    else // first pixel is R or B
    {
      if(FC(0, 0, filters) == 0)
      {
        ey = 0;
        ex = 0;
      }
      else
      {
        ey = 1;
        ex = 1;
      }
    }

// Main algorithm: Tile loop
//#ifdef _OPENMP
//#pragma omp parallel for shared(rawData,height,width,red,green,blue) private(top,left) schedule(dynamic)
//#endif
// code is openmp ready; just have to pull local tile variable declarations inside the tile loop

// Issue 1676
// use collapse(2) to collapse the 2 loops to one large loop, so there is better scaling
// WARNING from darktable: we don't use collapse(2) as this seems to trigger an issue in some versions of gcc
// 4.8
//         -- I (houz) added it back for testing on 13.04.2014 and disabled it for gcc 4.8.1 on 16.04.2014
#ifdef _OPENMP
#if !defined(__clang__) && defined(__GNUC__)                                                                 \
    && ((__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) == 40801)
#pragma omp for schedule(dynamic) nowait
#else
#pragma omp for schedule(dynamic) collapse(2) nowait
#endif
#endif
    for(top = winy - 16; top < winy + height; top += TS - 32)
      for(left = winx - 16; left < winx + width; left += TS - 32)
      {
        memset(nyquist, 0, sizeof(char) * TS * TSH);
        memset(rbint, 0, sizeof(float) * TS * TSH);
        // location of tile bottom edge
        int bottom = MIN(top + TS, winy + height + 16);
        // location of tile right edge
        int right = MIN(left + TS, winx + width + 16);
        // tile width  (=TS except for right edge of image)
        int rr1 = bottom - top;
        // tile height (=TS except for bottom edge of image)
        int cc1 = right - left;

        // tile vars
        // counters for pixel location in the image
        int row, col;
        // min and max row/column in the tile
        int rrmin, rrmax, ccmin, ccmax;
        // counters for pixel location within the tile
        int rr, cc;
        // color index 0=R, 1=G, 2=B
        int c;
        // pointer counters within the tile
        int indx, indx1;
        // dummy indices
        int i, j;

        // color ratios in up/down/left/right directions
        float cru, crd, crl, crr;
        // adaptive weights for vertical/horizontal/plus/minus directions
        float vwt, hwt /*, pwt, mwt*/;
#ifndef __SSE2__
        float pwt, mwt;
#endif
        // vertical and horizontal G interpolations
        float Gintv, Ginth;
#ifndef __SSE2__
        // G interpolated in vert/hor directions using adaptive ratios
        float guar, gdar, glar, grar;
        // G interpolated in vert/hor directions using Hamilton-Adams method
        float guha, gdha, glha, grha;
        // interpolated G from fusing left/right or up/down
        float /*Ginthar,*/ Ginthha, /*Gintvar,*/ Gintvha;
        // color difference (G-R or G-B) variance in up/down/left/right directions
        float Dgrbvvaru, Dgrbvvard, Dgrbhvarl, Dgrbhvarr;

        float uave, dave, lave, rave;
#endif

        // color difference variances in vertical and horizontal directions
        float vcdvar, hcdvar /*, vcdvar1, hcdvar1, hcdaltvar, vcdaltvar*/;
#ifndef __SSE2__
        float vcdvar1, hcdvar1, hcdaltvar, vcdaltvar;
        // adaptive interpolation weight using variance of color differences
        float varwt; // 639 - 644
        // adaptive interpolation weight using difference of left-right and up-down G interpolations
        float diffwt; // 640 - 644
#endif
        // alternative adaptive weight for combining horizontal/vertical interpolations
        float hvwtalt; // 745 - 748
        // interpolation of G in four directions
        float gu, gd, gl, gr;
        // variance of G in vertical/horizontal directions
        float gvarh, gvarv;

        // Nyquist texture test
        float nyqtest; // 658 - 681
        // accumulators for Nyquist texture interpolation
        float sumh, sumv, sumsqh, sumsqv, areawt;

#ifndef __SSE2__
        // color ratios in diagonal directions
        float crse, crnw, crne, crsw;
        // color differences in diagonal directions
        float rbse, rbnw, rbne, rbsw;
        // adaptive weights for combining diagonal interpolations
        float wtse, wtnw, wtsw, wtne;
        // alternate weight for combining diagonal interpolations
        float pmwtalt; // 885 - 888
        // variance of R-B in plus/minus directions
        float rbvarm; // 843 - 848
#endif

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // rgb from input CFA data
        // rgb values should be floating point number between 0 and 1
        // after white balance multipliers are applied
        // a 16 pixel border is added to each side of the image

        // bookkeeping for borders
        if(top < winy)
        {
          rrmin = 16;
        }
        else
        {
          rrmin = 0;
        }
        if(left < winx)
        {
          ccmin = 16;
        }
        else
        {
          ccmin = 0;
        }
        if(bottom > (winy + height))
        {
          rrmax = winy + height - top;
        }
        else
        {
          rrmax = rr1;
        }
        if(right > (winx + width))
        {
          ccmax = winx + width - left;
        }
        else
        {
          ccmax = cc1;
        }

#ifdef __SSE2__
        //         const __m128 c65535v = _mm_set1_ps( 65535.0f );
        __m128 tempv;
        for(rr = rrmin; rr < rrmax; rr++)
        {
          for(row = rr + top, cc = ccmin; cc < ccmax - 3; cc += 4)
          {
            indx1 = rr * TS + cc;
            tempv = LVFU(in[row * width + (cc + left)]);
            _mm_store_ps(&cfa[indx1], tempv);
            _mm_store_ps(&rgbgreen[indx1], tempv);
          }
          for(; cc < ccmax; cc++)
          {
            indx1 = rr * TS + cc;
            cfa[indx1] = (in[row * width + (cc + left)]);
            if(FC(rr, cc, filters) == 1) rgbgreen[indx1] = cfa[indx1];
          }
        }
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // fill borders
        if(rrmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = ccmin, row = 32 - rr + top; cc < ccmax; cc++)
            {
              cfa[rr * TS + cc] = (in[row * width + (cc + left)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + cc] = cfa[rr * TS + cc];
            }
        }
        if(rrmax < rr1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = ccmin; cc < ccmax; cc += 4)
            {
              indx1 = (rrmax + rr) * TS + cc;
              tempv = LVFU(in[(winy + height - rr - 2) * width + (left + cc)]);
              _mm_store_ps(&cfa[indx1], tempv);
              _mm_store_ps(&rgbgreen[indx1], tempv);
            }
        }

        if(ccmin > 0)
        {
          for(rr = rrmin; rr < rrmax; rr++)
            for(cc = 0, row = rr + top; cc < 16; cc++)
            {
              cfa[rr * TS + cc] = (in[row * width + (32 - cc + left)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + cc] = cfa[rr * TS + cc];
            }
        }

        if(ccmax < cc1)
        {
          for(rr = rrmin; rr < rrmax; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[rr * TS + ccmax + cc] = in[(top + rr) * width + (winx + width - cc - 2)];
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + ccmax + cc] = cfa[rr * TS + ccmax + cc];
            }
        }
        // also, fill the image corners
        if(rrmin > 0 && ccmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc += 4)
            {
              indx1 = (rr)*TS + cc;
              tempv = LVFU(in[(winy + 32 - rr) * width + (winx + 32 - cc)]);
              _mm_store_ps(&cfa[indx1], tempv);
              _mm_store_ps(&rgbgreen[indx1], tempv);
            }
        }
        if(rrmax < rr1 && ccmax < cc1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc += 4)
            {
              indx1 = (rrmax + rr) * TS + ccmax + cc;
              tempv = LVFU(in[(winy + height - rr - 2) * width + (winx + width - cc - 2)]);
              _mm_storeu_ps(&cfa[indx1], tempv);
              _mm_storeu_ps(&rgbgreen[indx1], tempv);
            }
        }
        if(rrmin > 0 && ccmax < cc1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {

              cfa[(rr)*TS + ccmax + cc] = (in[(winy + 32 - rr) * width + (winx + width - cc - 2)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rr)*TS + ccmax + cc] = cfa[(rr)*TS + ccmax + cc];
            }
        }
        if(rrmax < rr1 && ccmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[(rrmax + rr) * TS + cc] = (in[(winy + height - rr - 2) * width + (winx + 32 - cc)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rrmax + rr) * TS + cc] = cfa[(rrmax + rr) * TS + cc];
            }
        }

#else
        for(rr = rrmin; rr < rrmax; rr++)
          for(row = rr + top, cc = ccmin; cc < ccmax; cc++)
          {
            indx1 = rr * TS + cc;
            cfa[indx1] = (in[row * width + (cc + left)]);
            if(FC(rr, cc, filters) == 1) rgbgreen[indx1] = cfa[indx1];
          }

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // fill borders
        if(rrmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = ccmin, row = 32 - rr + top; cc < ccmax; cc++)
            {
              cfa[rr * TS + cc] = (in[row * width + (cc + left)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + cc] = cfa[rr * TS + cc];
            }
        }
        if(rrmax < rr1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = ccmin; cc < ccmax; cc++)
            {
              cfa[(rrmax + rr) * TS + cc] = (in[(winy + height - rr - 2) * width + (left + cc)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rrmax + rr) * TS + cc] = cfa[(rrmax + rr) * TS + cc];
            }
        }
        if(ccmin > 0)
        {
          for(rr = rrmin; rr < rrmax; rr++)
            for(cc = 0, row = rr + top; cc < 16; cc++)
            {
              cfa[rr * TS + cc] = (in[row * width + (32 - cc + left)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + cc] = cfa[rr * TS + cc];
            }
        }
        if(ccmax < cc1)
        {
          for(rr = rrmin; rr < rrmax; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[rr * TS + ccmax + cc] = (in[(top + rr) * width + (winx + width - cc - 2)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[rr * TS + ccmax + cc] = cfa[rr * TS + ccmax + cc];
            }
        }

        // also, fill the image corners
        if(rrmin > 0 && ccmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[(rr)*TS + cc] = (in[(winy + 32 - rr) * width + (winx + 32 - cc)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rr)*TS + cc] = cfa[(rr)*TS + cc];
            }
        }
        if(rrmax < rr1 && ccmax < cc1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[(rrmax + rr) * TS + ccmax + cc]
                  = (in[(winy + height - rr - 2) * width + (winx + width - cc - 2)]);
              if(FC(rr, cc, filters) == 1)
                rgbgreen[(rrmax + rr) * TS + ccmax + cc] = cfa[(rrmax + rr) * TS + ccmax + cc];
            }
        }
        if(rrmin > 0 && ccmax < cc1)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[(rr)*TS + ccmax + cc] = (in[(winy + 32 - rr) * width + (winx + width - cc - 2)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rr)*TS + ccmax + cc] = cfa[(rr)*TS + ccmax + cc];
            }
        }
        if(rrmax < rr1 && ccmin > 0)
        {
          for(rr = 0; rr < 16; rr++)
            for(cc = 0; cc < 16; cc++)
            {
              cfa[(rrmax + rr) * TS + cc] = (in[(winy + height - rr - 2) * width + (winx + 32 - cc)]);
              if(FC(rr, cc, filters) == 1) rgbgreen[(rrmax + rr) * TS + cc] = cfa[(rrmax + rr) * TS + cc];
            }
        }
#endif

// end of border fill
// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
#ifdef __SSE2__
        __m128 delhv, delvv;
        const __m128 epsv = _mm_set1_ps(eps);

        for(rr = 2; rr < rr1 - 2; rr++)
        {
          for(cc = 0, indx = (rr)*TS + cc; cc < cc1; cc += 4, indx += 4)
          {
            delhv = vabsf(LVFU(cfa[indx + 1]) - LVFU(cfa[indx - 1]));
            delvv = vabsf(LVF(cfa[indx + v1]) - LVF(cfa[indx - v1]));
            _mm_store_ps(&dirwts1[indx], epsv + vabsf(LVFU(cfa[indx + 2]) - LVF(cfa[indx]))
                                         + vabsf(LVF(cfa[indx]) - LVFU(cfa[indx - 2])) + delhv);
            delhv = delhv * delhv;
            _mm_store_ps(&dirwts0[indx], epsv + vabsf(LVF(cfa[indx + v2]) - LVF(cfa[indx]))
                                         + vabsf(LVF(cfa[indx]) - LVF(cfa[indx - v2])) + delvv);
            delvv = delvv * delvv;
            _mm_store_ps(&delhvsqsum[indx], delhv + delvv);
          }
        }
#else
        // horizontal and vedrtical gradient
        float delh, delv;
        for(rr = 2; rr < rr1 - 2; rr++)
          for(cc = 2, indx = (rr)*TS + cc; cc < cc1 - 2; cc++, indx++)
          {
            delh = fabsf(cfa[indx + 1] - cfa[indx - 1]);
            delv = fabsf(cfa[indx + v1] - cfa[indx - v1]);
            dirwts0[indx] = eps + fabsf(cfa[indx + v2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - v2])
                            + delv;
            dirwts1[indx] = eps + fabsf(cfa[indx + 2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - 2])
                            + delh; //+fabsf(cfa[indx+2]-cfa[indx-2]);
            delhvsqsum[indx] = SQR(delh) + SQR(delv);
          }
#endif

#ifdef __SSE2__
        __m128 Dgrbsq1pv, Dgrbsq1mv, temp2v;
        for(rr = 6; rr < rr1 - 6; rr++)
        {
          if((FC(rr, 2, filters) & 1) == 0)
          {
            for(cc = 6, indx = (rr)*TS + cc; cc < cc1 - 6; cc += 8, indx += 8)
            {
              tempv = LC2VFU(cfa[indx + 1]);
              Dgrbsq1pv
                  = (SQRV(tempv - LC2VFU(cfa[indx + 1 - p1])) + SQRV(tempv - LC2VFU(cfa[indx + 1 + p1])));
              _mm_storeu_ps(&delp[indx >> 1], vabsf(LC2VFU(cfa[indx + p1]) - LC2VFU(cfa[indx - p1])));
              _mm_storeu_ps(&delm[indx >> 1], vabsf(LC2VFU(cfa[indx + m1]) - LC2VFU(cfa[indx - m1])));
              Dgrbsq1mv
                  = (SQRV(tempv - LC2VFU(cfa[indx + 1 - m1])) + SQRV(tempv - LC2VFU(cfa[indx + 1 + m1])));
              _mm_storeu_ps(&Dgrbsq1m[indx >> 1], Dgrbsq1mv);
              _mm_storeu_ps(&Dgrbsq1p[indx >> 1], Dgrbsq1pv);
            }
          }
          else
          {
            for(cc = 6, indx = (rr)*TS + cc; cc < cc1 - 6; cc += 8, indx += 8)
            {
              tempv = LC2VFU(cfa[indx]);
              Dgrbsq1pv = (SQRV(tempv - LC2VFU(cfa[indx - p1])) + SQRV(tempv - LC2VFU(cfa[indx + p1])));
              _mm_storeu_ps(&delp[indx >> 1], vabsf(LC2VFU(cfa[indx + 1 + p1]) - LC2VFU(cfa[indx + 1 - p1])));
              _mm_storeu_ps(&delm[indx >> 1], vabsf(LC2VFU(cfa[indx + 1 + m1]) - LC2VFU(cfa[indx + 1 - m1])));
              Dgrbsq1mv = (SQRV(tempv - LC2VFU(cfa[indx - m1])) + SQRV(tempv - LC2VFU(cfa[indx + m1])));
              _mm_storeu_ps(&Dgrbsq1m[indx >> 1], Dgrbsq1mv);
              _mm_storeu_ps(&Dgrbsq1p[indx >> 1], Dgrbsq1pv);
            }
          }
        }
#else
        for(rr = 6; rr < rr1 - 6; rr++)
        {
          if((FC(rr, 2, filters) & 1) == 0)
          {
            for(cc = 6, indx = (rr)*TS + cc; cc < cc1 - 6; cc += 2, indx += 2)
            {
              delp[indx >> 1] = fabsf(cfa[indx + p1] - cfa[indx - p1]);
              delm[indx >> 1] = fabsf(cfa[indx + m1] - cfa[indx - m1]);
              Dgrbsq1p[indx >> 1]
                  = (SQR(cfa[indx + 1] - cfa[indx + 1 - p1]) + SQR(cfa[indx + 1] - cfa[indx + 1 + p1]));
              Dgrbsq1m[indx >> 1]
                  = (SQR(cfa[indx + 1] - cfa[indx + 1 - m1]) + SQR(cfa[indx + 1] - cfa[indx + 1 + m1]));
            }
          }
          else
          {
            for(cc = 6, indx = (rr)*TS + cc; cc < cc1 - 6; cc += 2, indx += 2)
            {
              Dgrbsq1p[indx >> 1] = (SQR(cfa[indx] - cfa[indx - p1]) + SQR(cfa[indx] - cfa[indx + p1]));
              Dgrbsq1m[indx >> 1] = (SQR(cfa[indx] - cfa[indx - m1]) + SQR(cfa[indx] - cfa[indx + m1]));
              delp[indx >> 1] = fabsf(cfa[indx + 1 + p1] - cfa[indx + 1 - p1]);
              delm[indx >> 1] = fabsf(cfa[indx + 1 + m1] - cfa[indx + 1 - m1]);
            }
          }
        }
#endif

// end of tile initialization
// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

// interpolate vertical and horizontal color differences

#ifdef __SSE2__
        __m128 sgnv, cruv, crdv, crlv, crrv, guhav, gdhav, glhav, grhav, hwtv, vwtv, Gintvhav, Ginthhav,
            guarv, gdarv, glarv, grarv;
        vmask clipmask;
        if(!(FC(4, 4, filters) & 1))
          sgnv = _mm_set_ps(1.0f, -1.0f, 1.0f, -1.0f);
        else
          sgnv = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f);

        __m128 zd5v = _mm_set1_ps(0.5f);
        __m128 onev = _mm_set1_ps(1.0f);
        __m128 arthreshv = _mm_set1_ps(arthresh);
        __m128 clip_pt8v = _mm_set1_ps(clip_pt8);

        for(rr = 4; rr < rr1 - 4; rr++)
        {
          sgnv = -sgnv;
          for(cc = 4, indx = rr * TS + cc; cc < cc1 - 7; cc += 4, indx += 4)
          {
            // color ratios in each cardinal direction
            cruv = LVF(cfa[indx - v1]) * (LVF(dirwts0[indx - v2]) + LVF(dirwts0[indx]))
                   / (LVF(dirwts0[indx - v2]) * (epsv + LVF(cfa[indx]))
                      + LVF(dirwts0[indx]) * (epsv + LVF(cfa[indx - v2])));
            crdv = LVF(cfa[indx + v1]) * (LVF(dirwts0[indx + v2]) + LVF(dirwts0[indx]))
                   / (LVF(dirwts0[indx + v2]) * (epsv + LVF(cfa[indx]))
                      + LVF(dirwts0[indx]) * (epsv + LVF(cfa[indx + v2])));
            crlv = LVFU(cfa[indx - 1]) * (LVFU(dirwts1[indx - 2]) + LVF(dirwts1[indx]))
                   / (LVFU(dirwts1[indx - 2]) * (epsv + LVF(cfa[indx]))
                      + LVF(dirwts1[indx]) * (epsv + LVFU(cfa[indx - 2])));
            crrv = LVFU(cfa[indx + 1]) * (LVFU(dirwts1[indx + 2]) + LVF(dirwts1[indx]))
                   / (LVFU(dirwts1[indx + 2]) * (epsv + LVF(cfa[indx]))
                      + LVF(dirwts1[indx]) * (epsv + LVFU(cfa[indx + 2])));

            guhav = LVF(cfa[indx - v1]) + zd5v * (LVF(cfa[indx]) - LVF(cfa[indx - v2]));
            gdhav = LVF(cfa[indx + v1]) + zd5v * (LVF(cfa[indx]) - LVF(cfa[indx + v2]));
            glhav = LVFU(cfa[indx - 1]) + zd5v * (LVF(cfa[indx]) - LVFU(cfa[indx - 2]));
            grhav = LVFU(cfa[indx + 1]) + zd5v * (LVF(cfa[indx]) - LVFU(cfa[indx + 2]));

            guarv = vself(vmaskf_lt(vabsf(onev - cruv), arthreshv), LVF(cfa[indx]) * cruv, guhav);
            gdarv = vself(vmaskf_lt(vabsf(onev - crdv), arthreshv), LVF(cfa[indx]) * crdv, gdhav);
            glarv = vself(vmaskf_lt(vabsf(onev - crlv), arthreshv), LVF(cfa[indx]) * crlv, glhav);
            grarv = vself(vmaskf_lt(vabsf(onev - crrv), arthreshv), LVF(cfa[indx]) * crrv, grhav);

            hwtv = LVFU(dirwts1[indx - 1]) / (LVFU(dirwts1[indx - 1]) + LVFU(dirwts1[indx + 1]));
            vwtv = LVF(dirwts0[indx - v1]) / (LVF(dirwts0[indx + v1]) + LVF(dirwts0[indx - v1]));

            // interpolated G via adaptive weights of cardinal evaluations
            Ginthhav = hwtv * grhav + (onev - hwtv) * glhav;
            Gintvhav = vwtv * gdhav + (onev - vwtv) * guhav;
            // interpolated color differences

            _mm_store_ps(&hcdalt[indx], sgnv * (Ginthhav - LVF(cfa[indx])));
            _mm_store_ps(&vcdalt[indx], sgnv * (Gintvhav - LVF(cfa[indx])));

            clipmask = vorm(vorm(vmaskf_gt(LVF(cfa[indx]), clip_pt8v), vmaskf_gt(Gintvhav, clip_pt8v)),
                            vmaskf_gt(Ginthhav, clip_pt8v));
            guarv = vself(clipmask, guhav, guarv);
            gdarv = vself(clipmask, gdhav, gdarv);
            glarv = vself(clipmask, glhav, glarv);
            grarv = vself(clipmask, grhav, grarv);
            _mm_store_ps(&vcd[indx], vself(clipmask, LVF(vcdalt[indx]),
                                           sgnv * ((vwtv * gdarv + (onev - vwtv) * guarv) - LVF(cfa[indx]))));
            _mm_store_ps(&hcd[indx], vself(clipmask, LVF(hcdalt[indx]),
                                           sgnv * ((hwtv * grarv + (onev - hwtv) * glarv) - LVF(cfa[indx]))));
            // differences of interpolations in opposite directions

            _mm_store_ps(&dgintv[indx], _mm_min_ps(SQRV(guhav - gdhav), SQRV(guarv - gdarv)));
            _mm_store_ps(&dginth[indx], _mm_min_ps(SQRV(glhav - grhav), SQRV(glarv - grarv)));
          }
        }
#else
        bool fcswitch;
        for(rr = 4; rr < rr1 - 4; rr++)
        {
          for(cc = 4, indx = rr * TS + cc, fcswitch = FC(rr, cc, filters) & 1; cc < cc1 - 4; cc++, indx++)
          {

            // color ratios in each cardinal direction
            cru = cfa[indx - v1] * (dirwts0[indx - v2] + dirwts0[indx])
                  / (dirwts0[indx - v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx - v2]));
            crd = cfa[indx + v1] * (dirwts0[indx + v2] + dirwts0[indx])
                  / (dirwts0[indx + v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx + v2]));
            crl = cfa[indx - 1] * (dirwts1[indx - 2] + dirwts1[indx])
                  / (dirwts1[indx - 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx - 2]));
            crr = cfa[indx + 1] * (dirwts1[indx + 2] + dirwts1[indx])
                  / (dirwts1[indx + 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx + 2]));

            guha = HCLIP(cfa[indx - v1]) + xdiv2f(cfa[indx] - cfa[indx - v2]);
            gdha = HCLIP(cfa[indx + v1]) + xdiv2f(cfa[indx] - cfa[indx + v2]);
            glha = HCLIP(cfa[indx - 1]) + xdiv2f(cfa[indx] - cfa[indx - 2]);
            grha = HCLIP(cfa[indx + 1]) + xdiv2f(cfa[indx] - cfa[indx + 2]);

            if(fabsf(1.0f - cru) < arthresh)
            {
              guar = cfa[indx] * cru;
            }
            else
            {
              guar = guha;
            }
            if(fabsf(1.0f - crd) < arthresh)
            {
              gdar = cfa[indx] * crd;
            }
            else
            {
              gdar = gdha;
            }
            if(fabsf(1.0f - crl) < arthresh)
            {
              glar = cfa[indx] * crl;
            }
            else
            {
              glar = glha;
            }
            if(fabsf(1.0f - crr) < arthresh)
            {
              grar = cfa[indx] * crr;
            }
            else
            {
              grar = grha;
            }

            hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
            vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

            // interpolated G via adaptive weights of cardinal evaluations
            Gintvha = vwt * gdha + (1.0f - vwt) * guha;
            Ginthha = hwt * grha + (1.0f - hwt) * glha;
            // interpolated color differences
            if(fcswitch)
            {
              vcd[indx] = cfa[indx] - (vwt * gdar + (1.0f - vwt) * guar);
              hcd[indx] = cfa[indx] - (hwt * grar + (1.0f - hwt) * glar);
              vcdalt[indx] = cfa[indx] - Gintvha;
              hcdalt[indx] = cfa[indx] - Ginthha;
            }
            else
            {
              // interpolated color differences
              vcd[indx] = (vwt * gdar + (1.0f - vwt) * guar) - cfa[indx];
              hcd[indx] = (hwt * grar + (1.0f - hwt) * glar) - cfa[indx];
              vcdalt[indx] = Gintvha - cfa[indx];
              hcdalt[indx] = Ginthha - cfa[indx];
            }
            fcswitch = !fcswitch;

            if(cfa[indx] > clip_pt8 || Gintvha > clip_pt8 || Ginthha > clip_pt8)
            {
              // use HA if highlights are (nearly) clipped
              guar = guha;
              gdar = gdha;
              glar = glha;
              grar = grha;
              vcd[indx] = vcdalt[indx];
              hcd[indx] = hcdalt[indx];
            }

            // differences of interpolations in opposite directions
            dgintv[indx] = MIN(SQR(guha - gdha), SQR(guar - gdar));
            dginth[indx] = MIN(SQR(glha - grha), SQR(glar - grar));
          }
        }
#endif

#ifdef __SSE2__
        __m128 hcdvarv, vcdvarv;
        __m128 hcdaltvarv, vcdaltvarv, hcdv, vcdv, hcdaltv, vcdaltv, sgn3v, Ginthv, Gintvv, hcdoldv, vcdoldv;
        __m128 threev = _mm_set1_ps(3.0f);
        __m128 clip_ptv = _mm_set1_ps(clip_pt);
        __m128 nsgnv;
        vmask hcdmask, vcdmask /*,tempmask*/;

        if(!(FC(4, 4, filters) & 1))
          sgnv = _mm_set_ps(1.0f, -1.0f, 1.0f, -1.0f);
        else
          sgnv = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f);

        sgn3v = threev * sgnv;
        for(rr = 4; rr < rr1 - 4; rr++)
        {
          nsgnv = sgnv;
          sgnv = -sgnv;
          sgn3v = -sgn3v;
          for(cc = 4, indx = rr * TS + cc, c = FC(rr, cc, filters) & 1; cc < cc1 - 4; cc += 4, indx += 4)
          {
            hcdv = LVF(hcd[indx]);
            hcdvarv = threev * (SQRV(LVFU(hcd[indx - 2])) + SQRV(hcdv) + SQRV(LVFU(hcd[indx + 2])))
                      - SQRV(LVFU(hcd[indx - 2]) + hcdv + LVFU(hcd[indx + 2]));
            hcdaltv = LVF(hcdalt[indx]);
            hcdaltvarv = threev
                         * (SQRV(LVFU(hcdalt[indx - 2])) + SQRV(hcdaltv) + SQRV(LVFU(hcdalt[indx + 2])))
                         - SQRV(LVFU(hcdalt[indx - 2]) + hcdaltv + LVFU(hcdalt[indx + 2]));
            vcdv = LVF(vcd[indx]);
            vcdvarv = threev * (SQRV(LVF(vcd[indx - v2])) + SQRV(vcdv) + SQRV(LVF(vcd[indx + v2])))
                      - SQRV(LVF(vcd[indx - v2]) + vcdv + LVF(vcd[indx + v2]));
            vcdaltv = LVF(vcdalt[indx]);
            vcdaltvarv = threev
                         * (SQRV(LVF(vcdalt[indx - v2])) + SQRV(vcdaltv) + SQRV(LVF(vcdalt[indx + v2])))
                         - SQRV(LVF(vcdalt[indx - v2]) + vcdaltv + LVF(vcdalt[indx + v2]));
            // choose the smallest variance; this yields a smoother interpolation
            hcdv = vself(vmaskf_lt(hcdaltvarv, hcdvarv), hcdaltv, hcdv);
            vcdv = vself(vmaskf_lt(vcdaltvarv, vcdvarv), vcdaltv, vcdv);

            Ginthv = sgnv * hcdv + LVF(cfa[indx]);
            temp2v = sgn3v * hcdv;
            hwtv = onev + temp2v / (epsv + Ginthv + LVF(cfa[indx]));
            hcdmask = vmaskf_gt(nsgnv * hcdv, ZEROV);
            hcdoldv = hcdv;
            tempv = nsgnv * (LVF(cfa[indx]) - ULIMV(Ginthv, LVFU(cfa[indx - 1]), LVFU(cfa[indx + 1])));
            hcdv = vself(vmaskf_lt((temp2v), -(LVF(cfa[indx]) + Ginthv)), tempv,
                         hwtv * hcdv + (onev - hwtv) * tempv);
            hcdv = vself(hcdmask, hcdv, hcdoldv);
            hcdv = vself(vmaskf_gt(Ginthv, clip_ptv), tempv, hcdv);
            _mm_store_ps(&hcd[indx], hcdv);

            Gintvv = sgnv * vcdv + LVF(cfa[indx]);
            temp2v = sgn3v * vcdv;
            vwtv = onev + temp2v / (epsv + Gintvv + LVF(cfa[indx]));
            vcdmask = vmaskf_gt(nsgnv * vcdv, ZEROV);
            vcdoldv = vcdv;
            tempv = nsgnv * (LVF(cfa[indx]) - ULIMV(Gintvv, LVF(cfa[indx - v1]), LVF(cfa[indx + v1])));
            vcdv = vself(vmaskf_lt((temp2v), -(LVF(cfa[indx]) + Gintvv)), tempv,
                         vwtv * vcdv + (onev - vwtv) * tempv);
            vcdv = vself(vcdmask, vcdv, vcdoldv);
            vcdv = vself(vmaskf_gt(Gintvv, clip_ptv), tempv, vcdv);
            _mm_store_ps(&vcd[indx], vcdv);
            _mm_storeu_ps(&cddiffsq[indx], SQRV(vcdv - hcdv));
          }
        }
#else
        for(rr = 4; rr < rr1 - 4; rr++)
        {
          // for (cc=4+(FC(rr,2,filters)&1),indx=rr*TS+cc,c=FC(rr,cc,filters); cc<cc1-4; cc+=2,indx+=2) {
          for(cc = 4, indx = rr * TS + cc, c = FC(rr, cc, filters) & 1; cc < cc1 - 4; cc++, indx++)
          {
            hcdvar = 3.0f * (SQR(hcd[indx - 2]) + SQR(hcd[indx]) + SQR(hcd[indx + 2]))
                     - SQR(hcd[indx - 2] + hcd[indx] + hcd[indx + 2]);
            hcdaltvar = 3.0f * (SQR(hcdalt[indx - 2]) + SQR(hcdalt[indx]) + SQR(hcdalt[indx + 2]))
                        - SQR(hcdalt[indx - 2] + hcdalt[indx] + hcdalt[indx + 2]);
            vcdvar = 3.0f * (SQR(vcd[indx - v2]) + SQR(vcd[indx]) + SQR(vcd[indx + v2]))
                     - SQR(vcd[indx - v2] + vcd[indx] + vcd[indx + v2]);
            vcdaltvar = 3.0f * (SQR(vcdalt[indx - v2]) + SQR(vcdalt[indx]) + SQR(vcdalt[indx + v2]))
                        - SQR(vcdalt[indx - v2] + vcdalt[indx] + vcdalt[indx + v2]);
            // choose the smallest variance; this yields a smoother interpolation
            if(hcdaltvar < hcdvar) hcd[indx] = hcdalt[indx];
            if(vcdaltvar < vcdvar) vcd[indx] = vcdalt[indx];

            // bound the interpolation in regions of high saturation
            if(c) // G site
            {
              Ginth = -hcd[indx] + cfa[indx]; // R or B
              Gintv = -vcd[indx] + cfa[indx]; // B or R

              if(hcd[indx] > 0)
              {
                if(3.0f * hcd[indx] > (Ginth + cfa[indx]))
                {
                  hcd[indx] = -ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx];
                }
                else
                {
                  hwt = 1.0f - 3.0f * hcd[indx] / (eps + Ginth + cfa[indx]);
                  hcd[indx] = hwt * hcd[indx]
                              + (1.0f - hwt) * (-ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx]);
                }
              }
              if(vcd[indx] > 0)
              {
                if(3.0f * vcd[indx] > (Gintv + cfa[indx]))
                {
                  vcd[indx] = -ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
                }
                else
                {
                  vwt = 1.0f - 3.0f * vcd[indx] / (eps + Gintv + cfa[indx]);
                  vcd[indx] = vwt * vcd[indx]
                              + (1.0f - vwt) * (-ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx]);
                }
              }

              if(Ginth > clip_pt)
                hcd[indx] = -ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx]; // for RT implementation
              if(Gintv > clip_pt) vcd[indx] = -ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
              // if (Ginth > pre_mul[c]) hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];//for dcraw
              // implementation
              // if (Gintv > pre_mul[c]) vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];
            }
            else // R or B site
            {

              Ginth = hcd[indx] + cfa[indx]; // interpolated G
              Gintv = vcd[indx] + cfa[indx];

              if(hcd[indx] < 0)
              {
                if(3.0f * hcd[indx] < -(Ginth + cfa[indx]))
                {
                  hcd[indx] = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx];
                }
                else
                {
                  hwt = 1.0f + 3.0f * hcd[indx] / (eps + Ginth + cfa[indx]);
                  hcd[indx] = hwt * hcd[indx]
                              + (1.0f - hwt) * (ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx]);
                }
              }
              if(vcd[indx] < 0)
              {
                if(3.0f * vcd[indx] < -(Gintv + cfa[indx]))
                {
                  vcd[indx] = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
                }
                else
                {
                  vwt = 1.0f + 3.0f * vcd[indx] / (eps + Gintv + cfa[indx]);
                  vcd[indx] = vwt * vcd[indx]
                              + (1.0f - vwt) * (ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx]);
                }
              }

              if(Ginth > clip_pt)
                hcd[indx] = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx]; // for RT implementation
              if(Gintv > clip_pt) vcd[indx] = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
              // if (Ginth > pre_mul[c]) hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];//for dcraw
              // implementation
              // if (Gintv > pre_mul[c]) vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
              cddiffsq[indx] = SQR(vcd[indx] - hcd[indx]);
            }
            c = !c;
          }
        }
#endif

#ifdef __SSE2__
        __m128 uavev, davev, lavev, ravev, Dgrbvvaruv, Dgrbvvardv, Dgrbhvarlv, Dgrbhvarrv, varwtv, diffwtv,
            vcdvar1v, hcdvar1v;
        __m128 epssqv = _mm_set1_ps(epssq);
        vmask decmask;
        for(rr = 6; rr < rr1 - 6; rr++)
        {
          for(cc = 6 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 6; cc += 8, indx += 8)
          {
            // compute color difference variances in cardinal directions
            tempv = LC2VFU(vcd[indx]);
            uavev = tempv + LC2VFU(vcd[indx - v1]) + LC2VFU(vcd[indx - v2]) + LC2VFU(vcd[indx - v3]);
            davev = tempv + LC2VFU(vcd[indx + v1]) + LC2VFU(vcd[indx + v2]) + LC2VFU(vcd[indx + v3]);
            Dgrbvvaruv = SQRV(tempv - uavev) + SQRV(LC2VFU(vcd[indx - v1]) - uavev)
                         + SQRV(LC2VFU(vcd[indx - v2]) - uavev) + SQRV(LC2VFU(vcd[indx - v3]) - uavev);
            Dgrbvvardv = SQRV(tempv - davev) + SQRV(LC2VFU(vcd[indx + v1]) - davev)
                         + SQRV(LC2VFU(vcd[indx + v2]) - davev) + SQRV(LC2VFU(vcd[indx + v3]) - davev);

            hwtv = LC2VFU(dirwts1[indx - 1]) / (LC2VFU(dirwts1[indx - 1]) + LC2VFU(dirwts1[indx + 1]));
            vwtv = LC2VFU(dirwts0[indx - v1]) / (LC2VFU(dirwts0[indx + v1]) + LC2VFU(dirwts0[indx - v1]));

            tempv = LC2VFU(hcd[indx]);
            lavev = tempv + LC2VFU(hcd[indx - 1]) + LC2VFU(hcd[indx - 2]) + LC2VFU(hcd[indx - 3]);
            ravev = tempv + LC2VFU(hcd[indx + 1]) + LC2VFU(hcd[indx + 2]) + LC2VFU(hcd[indx + 3]);
            Dgrbhvarlv = SQRV(tempv - lavev) + SQRV(LC2VFU(hcd[indx - 1]) - lavev)
                         + SQRV(LC2VFU(hcd[indx - 2]) - lavev) + SQRV(LC2VFU(hcd[indx - 3]) - lavev);
            Dgrbhvarrv = SQRV(tempv - ravev) + SQRV(LC2VFU(hcd[indx + 1]) - ravev)
                         + SQRV(LC2VFU(hcd[indx + 2]) - ravev) + SQRV(LC2VFU(hcd[indx + 3]) - ravev);


            vcdvarv = epssqv + vwtv * Dgrbvvardv + (onev - vwtv) * Dgrbvvaruv;
            hcdvarv = epssqv + hwtv * Dgrbhvarrv + (onev - hwtv) * Dgrbhvarlv;

            // compute fluctuations in up/down and left/right interpolations of colors
            Dgrbvvaruv = (LC2VFU(dgintv[indx])) + (LC2VFU(dgintv[indx - v1])) + (LC2VFU(dgintv[indx - v2]));
            Dgrbvvardv = (LC2VFU(dgintv[indx])) + (LC2VFU(dgintv[indx + v1])) + (LC2VFU(dgintv[indx + v2]));
            Dgrbhvarlv = (LC2VFU(dginth[indx])) + (LC2VFU(dginth[indx - 1])) + (LC2VFU(dginth[indx - 2]));
            Dgrbhvarrv = (LC2VFU(dginth[indx])) + (LC2VFU(dginth[indx + 1])) + (LC2VFU(dginth[indx + 2]));

            vcdvar1v = epssqv + vwtv * Dgrbvvardv + (onev - vwtv) * Dgrbvvaruv;
            hcdvar1v = epssqv + hwtv * Dgrbhvarrv + (onev - hwtv) * Dgrbhvarlv;

            // determine adaptive weights for G interpolation
            varwtv = hcdvarv / (vcdvarv + hcdvarv);
            diffwtv = hcdvar1v / (vcdvar1v + hcdvar1v);

            // if both agree on interpolation direction, choose the one with strongest directional
            // discrimination;
            // otherwise, choose the u/d and l/r difference fluctuation weights
            decmask = vandm(vmaskf_gt((zd5v - varwtv) * (zd5v - diffwtv), ZEROV),
                            vmaskf_lt(vabsf(zd5v - diffwtv), vabsf(zd5v - varwtv)));
            _mm_storeu_ps(&hvwt[indx >> 1], vself(decmask, varwtv, diffwtv));
          }
        }
#else
        for(rr = 6; rr < rr1 - 6; rr++)
        {
          for(cc = 6 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 6; cc += 2, indx += 2)
          {

            // compute color difference variances in cardinal directions

            uave = vcd[indx] + vcd[indx - v1] + vcd[indx - v2] + vcd[indx - v3];
            dave = vcd[indx] + vcd[indx + v1] + vcd[indx + v2] + vcd[indx + v3];
            lave = hcd[indx] + hcd[indx - 1] + hcd[indx - 2] + hcd[indx - 3];
            rave = hcd[indx] + hcd[indx + 1] + hcd[indx + 2] + hcd[indx + 3];

            Dgrbvvaru = SQR(vcd[indx] - uave) + SQR(vcd[indx - v1] - uave) + SQR(vcd[indx - v2] - uave)
                        + SQR(vcd[indx - v3] - uave);
            Dgrbvvard = SQR(vcd[indx] - dave) + SQR(vcd[indx + v1] - dave) + SQR(vcd[indx + v2] - dave)
                        + SQR(vcd[indx + v3] - dave);
            Dgrbhvarl = SQR(hcd[indx] - lave) + SQR(hcd[indx - 1] - lave) + SQR(hcd[indx - 2] - lave)
                        + SQR(hcd[indx - 3] - lave);
            Dgrbhvarr = SQR(hcd[indx] - rave) + SQR(hcd[indx + 1] - rave) + SQR(hcd[indx + 2] - rave)
                        + SQR(hcd[indx + 3] - rave);

            hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
            vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

            vcdvar = epssq + vwt * Dgrbvvard + (1.0f - vwt) * Dgrbvvaru;
            hcdvar = epssq + hwt * Dgrbhvarr + (1.0f - hwt) * Dgrbhvarl;

            // compute fluctuations in up/down and left/right interpolations of colors
            Dgrbvvaru = (dgintv[indx]) + (dgintv[indx - v1]) + (dgintv[indx - v2]);
            Dgrbvvard = (dgintv[indx]) + (dgintv[indx + v1]) + (dgintv[indx + v2]);
            Dgrbhvarl = (dginth[indx]) + (dginth[indx - 1]) + (dginth[indx - 2]);
            Dgrbhvarr = (dginth[indx]) + (dginth[indx + 1]) + (dginth[indx + 2]);

            vcdvar1 = epssq + vwt * Dgrbvvard + (1.0f - vwt) * Dgrbvvaru;
            hcdvar1 = epssq + hwt * Dgrbhvarr + (1.0f - hwt) * Dgrbhvarl;

            // determine adaptive weights for G interpolation
            varwt = hcdvar / (vcdvar + hcdvar);
            diffwt = hcdvar1 / (vcdvar1 + hcdvar1);

            // if both agree on interpolation direction, choose the one with strongest directional
            // discrimination;
            // otherwise, choose the u/d and l/r difference fluctuation weights
            if((0.5 - varwt) * (0.5 - diffwt) > 0 && fabsf(0.5 - diffwt) < fabsf(0.5 - varwt))
            {
              hvwt[indx >> 1] = varwt;
            }
            else
            {
              hvwt[indx >> 1] = diffwt;
            }

            // hvwt[indx]=varwt;
          }
        }

#endif
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // Nyquist test
        for(rr = 6; rr < rr1 - 6; rr++)
          for(cc = 6 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 6; cc += 2, indx += 2)
          {

            // nyquist texture test: ask if difference of vcd compared to hcd is larger or smaller than RGGB
            // gradients
            nyqtest = (gaussodd[0] * cddiffsq[indx]
                       + gaussodd[1] * (cddiffsq[(indx - m1)] + cddiffsq[(indx + p1)] + cddiffsq[(indx - p1)]
                                        + cddiffsq[(indx + m1)])
                       + gaussodd[2] * (cddiffsq[(indx - v2)] + cddiffsq[(indx - 2)] + cddiffsq[(indx + 2)]
                                        + cddiffsq[(indx + v2)])
                       + gaussodd[3] * (cddiffsq[(indx - m2)] + cddiffsq[(indx + p2)] + cddiffsq[(indx - p2)]
                                        + cddiffsq[(indx + m2)]));

            nyqtest -= nyqthresh
                       * (gaussgrad[0] * (delhvsqsum[indx])
                          + gaussgrad[1] * (delhvsqsum[indx - v1] + delhvsqsum[indx + 1]
                                            + delhvsqsum[indx - 1] + delhvsqsum[indx + v1])
                          + gaussgrad[2] * (delhvsqsum[indx - m1] + delhvsqsum[indx + p1]
                                            + delhvsqsum[indx - p1] + delhvsqsum[indx + m1])
                          + gaussgrad[3] * (delhvsqsum[indx - v2] + delhvsqsum[indx - 2]
                                            + delhvsqsum[indx + 2] + delhvsqsum[indx + v2])
                          + gaussgrad[4] * (delhvsqsum[indx - 2 * TS - 1] + delhvsqsum[indx - 2 * TS + 1]
                                            + delhvsqsum[indx - TS - 2] + delhvsqsum[indx - TS + 2]
                                            + delhvsqsum[indx + TS - 2] + delhvsqsum[indx + TS + 2]
                                            + delhvsqsum[indx + 2 * TS - 1] + delhvsqsum[indx + 2 * TS + 1])
                          + gaussgrad[5] * (delhvsqsum[indx - m2] + delhvsqsum[indx + p2]
                                            + delhvsqsum[indx - p2] + delhvsqsum[indx + m2]));


            if(nyqtest > 0) nyquist[indx >> 1] = 1; // nyquist=1 for nyquist region
          }

        unsigned int nyquisttemp;
        for(rr = 8; rr < rr1 - 8; rr++)
        {
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 8; cc += 2, indx += 2)
          {

            nyquisttemp
                = (nyquist[(indx - v2) >> 1] + nyquist[(indx - m1) >> 1] + nyquist[(indx + p1) >> 1]
                   + nyquist[(indx - 2) >> 1] + nyquist[indx >> 1] + nyquist[(indx + 2) >> 1]
                   + nyquist[(indx - p1) >> 1] + nyquist[(indx + m1) >> 1] + nyquist[(indx + v2) >> 1]);
            // if most of your neighbors are named Nyquist, it's likely that you're one too
            if(nyquisttemp > 4) nyquist[indx >> 1] = 1;
            // or not
            if(nyquisttemp < 4) nyquist[indx >> 1] = 0;
          }
        }
        // end of Nyquist test

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // in areas of Nyquist texture, do area interpolation
        for(rr = 8; rr < rr1 - 8; rr++)
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 8; cc += 2, indx += 2)
          {

            if(nyquist[indx >> 1])
            {
              // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
              // area interpolation

              sumh = sumv = sumsqh = sumsqv = areawt = 0;
              for(i = -6; i < 7; i += 2)
                for(j = -6; j < 7; j += 2)
                {
                  indx1 = (rr + i) * TS + cc + j;
                  if(nyquist[indx1 >> 1])
                  {
                    sumh += cfa[indx1] - xdiv2f(cfa[indx1 - 1] + cfa[indx1 + 1]);
                    sumv += cfa[indx1] - xdiv2f(cfa[indx1 - v1] + cfa[indx1 + v1]);
                    sumsqh += xdiv2f(SQR(cfa[indx1] - cfa[indx1 - 1]) + SQR(cfa[indx1] - cfa[indx1 + 1]));
                    sumsqv += xdiv2f(SQR(cfa[indx1] - cfa[indx1 - v1]) + SQR(cfa[indx1] - cfa[indx1 + v1]));
                    areawt += 1;
                  }
                }

              // horizontal and vertical color differences, and adaptive weight
              hcdvar = epssq + fabsf(areawt * sumsqh - sumh * sumh);
              vcdvar = epssq + fabsf(areawt * sumsqv - sumv * sumv);
              hvwt[indx >> 1] = hcdvar / (vcdvar + hcdvar);

              // end of area interpolation
              // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            }
          }

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // populate G at R/B sites
        for(rr = 8; rr < rr1 - 8; rr++)
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 8; cc += 2, indx += 2)
          {

            // first ask if one gets more directional discrimination from nearby B/R sites
            hvwtalt = xdivf(hvwt[(indx - m1) >> 1] + hvwt[(indx + p1) >> 1] + hvwt[(indx - p1) >> 1]
                            + hvwt[(indx + m1) >> 1],
                            2);
            //					hvwtalt =
            //0.25*(hvwt[(indx-m1)>>1]+hvwt[(indx+p1)>>1]+hvwt[(indx-p1)>>1]+hvwt[(indx+m1)>>1]);
            //					vo=fabsf(0.5-hvwt[indx>>1]);
            //					ve=fabsf(0.5-hvwtalt);
            if(fabsf(0.5f - hvwt[indx >> 1]) < fabsf(0.5f - hvwtalt))
            {
              hvwt[indx >> 1] = hvwtalt; // a better result was obtained from the neighbors
            }
            //					if (vo<ve) {hvwt[indx>>1]=hvwtalt;}//a better result was obtained from the neighbors



            Dgrb[0][indx >> 1] = (hcd[indx] * (1.0f - hvwt[indx >> 1])
                                  + vcd[indx] * hvwt[indx >> 1]); // evaluate color differences
            // if (hvwt[indx]<0.5) Dgrb[indx][0]=hcd[indx];
            // if (hvwt[indx]>0.5) Dgrb[indx][0]=vcd[indx];
            rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1]; // evaluate G (finally!)

            // local curvature in G (preparation for nyquist refinement step)
            if(nyquist[indx >> 1])
            {
              Dgrb2[indx >> 1].h = SQR(rgbgreen[indx] - xdiv2f(rgbgreen[indx - 1] + rgbgreen[indx + 1]));
              Dgrb2[indx >> 1].v = SQR(rgbgreen[indx] - xdiv2f(rgbgreen[indx - v1] + rgbgreen[indx + v1]));
            }
            else
            {
              Dgrb2[indx >> 1].h = Dgrb2[indx >> 1].v = 0;
            }
          }

        // end of standard interpolation
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // refine Nyquist areas using G curvatures

        for(rr = 8; rr < rr1 - 8; rr++)
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc; cc < cc1 - 8; cc += 2, indx += 2)
          {

            if(nyquist[indx >> 1])
            {
              // local averages (over Nyquist pixels only) of G curvature squared
              gvarh = epssq + (gquinc[0] * Dgrb2[indx >> 1].h
                               + gquinc[1] * (Dgrb2[(indx - m1) >> 1].h + Dgrb2[(indx + p1) >> 1].h
                                              + Dgrb2[(indx - p1) >> 1].h + Dgrb2[(indx + m1) >> 1].h)
                               + gquinc[2] * (Dgrb2[(indx - v2) >> 1].h + Dgrb2[(indx - 2) >> 1].h
                                              + Dgrb2[(indx + 2) >> 1].h + Dgrb2[(indx + v2) >> 1].h)
                               + gquinc[3] * (Dgrb2[(indx - m2) >> 1].h + Dgrb2[(indx + p2) >> 1].h
                                              + Dgrb2[(indx - p2) >> 1].h + Dgrb2[(indx + m2) >> 1].h));
              gvarv = epssq + (gquinc[0] * Dgrb2[indx >> 1].v
                               + gquinc[1] * (Dgrb2[(indx - m1) >> 1].v + Dgrb2[(indx + p1) >> 1].v
                                              + Dgrb2[(indx - p1) >> 1].v + Dgrb2[(indx + m1) >> 1].v)
                               + gquinc[2] * (Dgrb2[(indx - v2) >> 1].v + Dgrb2[(indx - 2) >> 1].v
                                              + Dgrb2[(indx + 2) >> 1].v + Dgrb2[(indx + v2) >> 1].v)
                               + gquinc[3] * (Dgrb2[(indx - m2) >> 1].v + Dgrb2[(indx + p2) >> 1].v
                                              + Dgrb2[(indx - p2) >> 1].v + Dgrb2[(indx + m2) >> 1].v));
              // use the results as weights for refined G interpolation
              Dgrb[0][indx >> 1] = (hcd[indx] * gvarv + vcd[indx] * gvarh) / (gvarv + gvarh);
              rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1];
            }
          }

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

// diagonal interpolation correction

#ifdef __SSE2__
        __m128 rbsev, rbnwv, rbnev, rbswv, cfav, rbmv, rbpv, temp1v, wtv;
        __m128 wtsev, wtnwv, wtnev, wtswv, rbvarmv;
        __m128 gausseven0v = _mm_set1_ps(gausseven[0]);
        __m128 gausseven1v = _mm_set1_ps(gausseven[1]);
        __m128 twov = _mm_set1_ps(2.0f);
#endif
        for(rr = 8; rr < rr1 - 8; rr++)
        {
#ifdef __SSE2__
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, indx1 = indx >> 1; cc < cc1 - 8;
              cc += 8, indx += 8, indx1 += 4)
          {

            // diagonal color ratios
            cfav = LC2VFU(cfa[indx]);

            temp1v = LC2VFU(cfa[indx + m1]);
            temp2v = LC2VFU(cfa[indx + m2]);
            rbsev = (temp1v + temp1v) / (epsv + cfav + temp2v);
            rbsev = vself(vmaskf_lt(vabsf(onev - rbsev), arthreshv), cfav * rbsev,
                          temp1v + zd5v * (cfav - temp2v));

            temp1v = LC2VFU(cfa[indx - m1]);
            temp2v = LC2VFU(cfa[indx - m2]);
            rbnwv = (temp1v + temp1v) / (epsv + cfav + temp2v);
            rbnwv = vself(vmaskf_lt(vabsf(onev - rbnwv), arthreshv), cfav * rbnwv,
                          temp1v + zd5v * (cfav - temp2v));

            temp1v = epsv + LVFU(delm[indx1]);
            wtsev = temp1v + LVFU(delm[(indx + m1) >> 1])
                    + LVFU(delm[(indx + m2) >> 1]); // same as for wtu,wtd,wtl,wtr
            wtnwv = temp1v + LVFU(delm[(indx - m1) >> 1]) + LVFU(delm[(indx - m2) >> 1]);

            rbmv = (wtsev * rbnwv + wtnwv * rbsev) / (wtsev + wtnwv);

            temp1v = ULIMV(rbmv, LC2VFU(cfa[indx - m1]), LC2VFU(cfa[indx + m1]));
            wtv = twov * (cfav - rbmv) / (epsv + rbmv + cfav);
            temp2v = wtv * rbmv + (onev - wtv) * temp1v;

            temp2v = vself(vmaskf_lt(rbmv + rbmv, cfav), temp1v, temp2v);
            temp2v = vself(vmaskf_lt(rbmv, cfav), temp2v, rbmv);
            _mm_storeu_ps(&rbm[indx1],
                          vself(vmaskf_gt(temp2v, clip_ptv),
                                ULIMV(temp2v, LC2VFU(cfa[indx - m1]), LC2VFU(cfa[indx + m1])), temp2v));


            temp1v = LC2VFU(cfa[indx + p1]);
            temp2v = LC2VFU(cfa[indx + p2]);
            rbnev = (temp1v + temp1v) / (epsv + cfav + temp2v);
            rbnev = vself(vmaskf_lt(vabsf(onev - rbnev), arthreshv), cfav * rbnev,
                          temp1v + zd5v * (cfav - temp2v));

            temp1v = LC2VFU(cfa[indx - p1]);
            temp2v = LC2VFU(cfa[indx - p2]);
            rbswv = (temp1v + temp1v) / (epsv + cfav + temp2v);
            rbswv = vself(vmaskf_lt(vabsf(onev - rbswv), arthreshv), cfav * rbswv,
                          temp1v + zd5v * (cfav - temp2v));

            temp1v = epsv + LVFU(delp[indx1]);
            wtnev = temp1v + LVFU(delp[(indx + p1) >> 1]) + LVFU(delp[(indx + p2) >> 1]);
            wtswv = temp1v + LVFU(delp[(indx - p1) >> 1]) + LVFU(delp[(indx - p2) >> 1]);

            rbpv = (wtnev * rbswv + wtswv * rbnev) / (wtnev + wtswv);

            temp1v = ULIMV(rbpv, LC2VFU(cfa[indx - p1]), LC2VFU(cfa[indx + p1]));
            wtv = twov * (cfav - rbpv) / (epsv + rbpv + cfav);
            temp2v = wtv * rbpv + (onev - wtv) * temp1v;

            temp2v = vself(vmaskf_lt(rbpv + rbpv, cfav), temp1v, temp2v);
            temp2v = vself(vmaskf_lt(rbpv, cfav), temp2v, rbpv);
            _mm_storeu_ps(&rbp[indx1],
                          vself(vmaskf_gt(temp2v, clip_ptv),
                                ULIMV(temp2v, LC2VFU(cfa[indx - p1]), LC2VFU(cfa[indx + p1])), temp2v));



            rbvarmv = epssqv
                      + (gausseven0v * (LVFU(Dgrbsq1m[(indx - v1) >> 1]) + LVFU(Dgrbsq1m[(indx - 1) >> 1])
                                        + LVFU(Dgrbsq1m[(indx + 1) >> 1]) + LVFU(Dgrbsq1m[(indx + v1) >> 1]))
                         + gausseven1v
                           * (LVFU(Dgrbsq1m[(indx - v2 - 1) >> 1]) + LVFU(Dgrbsq1m[(indx - v2 + 1) >> 1])
                              + LVFU(Dgrbsq1m[(indx - 2 - v1) >> 1]) + LVFU(Dgrbsq1m[(indx + 2 - v1) >> 1])
                              + LVFU(Dgrbsq1m[(indx - 2 + v1) >> 1]) + LVFU(Dgrbsq1m[(indx + 2 + v1) >> 1])
                              + LVFU(Dgrbsq1m[(indx + v2 - 1) >> 1]) + LVFU(Dgrbsq1m[(indx + v2 + 1) >> 1])));
            _mm_storeu_ps(
                &pmwt[indx1],
                rbvarmv
                / ((epssqv
                    + (gausseven0v * (LVFU(Dgrbsq1p[(indx - v1) >> 1]) + LVFU(Dgrbsq1p[(indx - 1) >> 1])
                                      + LVFU(Dgrbsq1p[(indx + 1) >> 1]) + LVFU(Dgrbsq1p[(indx + v1) >> 1]))
                       + gausseven1v
                         * (LVFU(Dgrbsq1p[(indx - v2 - 1) >> 1]) + LVFU(Dgrbsq1p[(indx - v2 + 1) >> 1])
                            + LVFU(Dgrbsq1p[(indx - 2 - v1) >> 1]) + LVFU(Dgrbsq1p[(indx + 2 - v1) >> 1])
                            + LVFU(Dgrbsq1p[(indx - 2 + v1) >> 1]) + LVFU(Dgrbsq1p[(indx + 2 + v1) >> 1])
                            + LVFU(Dgrbsq1p[(indx + v2 - 1) >> 1]) + LVFU(Dgrbsq1p[(indx + v2 + 1) >> 1]))))
                   + rbvarmv));
          }

#else
          for(cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, indx1 = indx >> 1; cc < cc1 - 8;
              cc += 2, indx += 2, indx1++)
          {

            // diagonal color ratios
            crse = xmul2f(cfa[indx + m1]) / (eps + cfa[indx] + (cfa[indx + m2]));
            crnw = xmul2f(cfa[indx - m1]) / (eps + cfa[indx] + (cfa[indx - m2]));
            crne = xmul2f(cfa[indx + p1]) / (eps + cfa[indx] + (cfa[indx + p2]));
            crsw = xmul2f(cfa[indx - p1]) / (eps + cfa[indx] + (cfa[indx - p2]));

            // assign B/R at R/B sites
            if(fabsf(1.0f - crse) < arthresh)
              rbse = cfa[indx] * crse; // use this if more precise diag interp is necessary
            else
              rbse = (cfa[indx + m1]) + xdiv2f(cfa[indx] - cfa[indx + m2]);
            if(fabsf(1.0f - crnw) < arthresh)
              rbnw = cfa[indx] * crnw;
            else
              rbnw = (cfa[indx - m1]) + xdiv2f(cfa[indx] - cfa[indx - m2]);
            if(fabsf(1.0f - crne) < arthresh)
              rbne = cfa[indx] * crne;
            else
              rbne = (cfa[indx + p1]) + xdiv2f(cfa[indx] - cfa[indx + p2]);
            if(fabsf(1.0f - crsw) < arthresh)
              rbsw = cfa[indx] * crsw;
            else
              rbsw = (cfa[indx - p1]) + xdiv2f(cfa[indx] - cfa[indx - p2]);

            wtse = eps + delm[indx1] + delm[(indx + m1) >> 1]
                   + delm[(indx + m2) >> 1]; // same as for wtu,wtd,wtl,wtr
            wtnw = eps + delm[indx1] + delm[(indx - m1) >> 1] + delm[(indx - m2) >> 1];
            wtne = eps + delp[indx1] + delp[(indx + p1) >> 1] + delp[(indx + p2) >> 1];
            wtsw = eps + delp[indx1] + delp[(indx - p1) >> 1] + delp[(indx - p2) >> 1];


            rbm[indx1] = (wtse * rbnw + wtnw * rbse) / (wtse + wtnw);
            rbp[indx1] = (wtne * rbsw + wtsw * rbne) / (wtne + wtsw);
            /*
                      rbvarp = epssq +
               (gausseven[0]*(Dgrbsq1[indx-v1].p+Dgrbsq1[indx-1].p+Dgrbsq1[indx+1].p+Dgrbsq1[indx+v1].p) +
                              gausseven[1]*(Dgrbsq1[indx-v2-1].p+Dgrbsq1[indx-v2+1].p+Dgrbsq1[indx-2-v1].p+Dgrbsq1[indx+2-v1].p+
                                      Dgrbsq1[indx-2+v1].p+Dgrbsq1[indx+2+v1].p+Dgrbsq1[indx+v2-1].p+Dgrbsq1[indx+v2+1].p));
            */
            rbvarm = epssq
                     + (gausseven[0] * (Dgrbsq1m[(indx - v1) >> 1] + Dgrbsq1m[(indx - 1) >> 1]
                                        + Dgrbsq1m[(indx + 1) >> 1] + Dgrbsq1m[(indx + v1) >> 1])
                        + gausseven[1] * (Dgrbsq1m[(indx - v2 - 1) >> 1] + Dgrbsq1m[(indx - v2 + 1) >> 1]
                                          + Dgrbsq1m[(indx - 2 - v1) >> 1] + Dgrbsq1m[(indx + 2 - v1) >> 1]
                                          + Dgrbsq1m[(indx - 2 + v1) >> 1] + Dgrbsq1m[(indx + 2 + v1) >> 1]
                                          + Dgrbsq1m[(indx + v2 - 1) >> 1] + Dgrbsq1m[(indx + v2 + 1) >> 1]));
            pmwt[indx1]
                = rbvarm
                  / ((epssq + (gausseven[0] * (Dgrbsq1p[(indx - v1) >> 1] + Dgrbsq1p[(indx - 1) >> 1]
                                               + Dgrbsq1p[(indx + 1) >> 1] + Dgrbsq1p[(indx + v1) >> 1])
                               + gausseven[1]
                                 * (Dgrbsq1p[(indx - v2 - 1) >> 1] + Dgrbsq1p[(indx - v2 + 1) >> 1]
                                    + Dgrbsq1p[(indx - 2 - v1) >> 1] + Dgrbsq1p[(indx + 2 - v1) >> 1]
                                    + Dgrbsq1p[(indx - 2 + v1) >> 1] + Dgrbsq1p[(indx + 2 + v1) >> 1]
                                    + Dgrbsq1p[(indx + v2 - 1) >> 1] + Dgrbsq1p[(indx + v2 + 1) >> 1])))
                     + rbvarm);

            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            // bound the interpolation in regions of high saturation
            if(rbp[indx1] < cfa[indx])
            {
              if(xmul2f(rbp[indx1]) < cfa[indx])
              {
                rbp[indx1] = ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
              }
              else
              {
                pwt = xmul2f(cfa[indx] - rbp[indx1]) / (eps + rbp[indx1] + cfa[indx]);
                rbp[indx1] = pwt * rbp[indx1]
                             + (1.0f - pwt) * ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
              }
            }
            if(rbm[indx1] < cfa[indx])
            {
              if(xmul2f(rbm[indx1]) < cfa[indx])
              {
                rbm[indx1] = ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
              }
              else
              {
                mwt = xmul2f(cfa[indx] - rbm[indx1]) / (eps + rbm[indx1] + cfa[indx]);
                rbm[indx1] = mwt * rbm[indx1]
                             + (1.0f - mwt) * ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
              }
            }

            if(rbp[indx1] > clip_pt)
              rbp[indx1] = ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]); // for RT implementation
            if(rbm[indx1] > clip_pt) rbm[indx1] = ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
            // c=2-FC(rr,cc,filters);//for dcraw implementation
            // if (rbp[indx] > pre_mul[c]) rbp[indx]=ULIM(rbp[indx],cfa[indx-p1],cfa[indx+p1]);
            // if (rbm[indx] > pre_mul[c]) rbm[indx]=ULIM(rbm[indx],cfa[indx-m1],cfa[indx+m1]);
            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            // rbint[indx] = 0.5*(cfa[indx] + (rbp*rbvarm+rbm*rbvarp)/(rbvarp+rbvarm));//this is R+B,
            // interpolated
          }
#endif
        }

#ifdef __SSE2__
        __m128 pmwtaltv;
        __m128 zd25v = _mm_set1_ps(0.25f);
#endif
        for(rr = 10; rr < rr1 - 10; rr++)
#ifdef __SSE2__
          for(cc = 10 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, indx1 = indx >> 1; cc < cc1 - 10;
              cc += 8, indx += 8, indx1 += 4)
          {

            // first ask if one gets more directional discrimination from nearby B/R sites
            pmwtaltv = zd25v * (LVFU(pmwt[(indx - m1) >> 1]) + LVFU(pmwt[(indx + p1) >> 1])
                                + LVFU(pmwt[(indx - p1) >> 1]) + LVFU(pmwt[(indx + m1) >> 1]));
            tempv = LVFU(pmwt[indx1]);
            tempv = vself(vmaskf_lt(vabsf(zd5v - tempv), vabsf(zd5v - pmwtaltv)), pmwtaltv, tempv);
            _mm_storeu_ps(&pmwt[indx1], tempv);
            _mm_storeu_ps(&rbint[indx1], zd5v * (LC2VFU(cfa[indx]) + LVFU(rbm[indx1]) * (onev - tempv)
                                                 + LVFU(rbp[indx1]) * tempv));
          }

#else
          for(cc = 10 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, indx1 = indx >> 1; cc < cc1 - 10;
              cc += 2, indx += 2, indx1++)
          {

            // first ask if one gets more directional discrimination from nearby B/R sites
            pmwtalt = xdivf(pmwt[(indx - m1) >> 1] + pmwt[(indx + p1) >> 1] + pmwt[(indx - p1) >> 1]
                            + pmwt[(indx + m1) >> 1],
                            2);
            if(fabsf(0.5 - pmwt[indx1]) < fabsf(0.5 - pmwtalt))
            {
              pmwt[indx1] = pmwtalt; // a better result was obtained from the neighbors
            }

            rbint[indx1] = xdiv2f(cfa[indx] + rbm[indx1] * (1.0f - pmwt[indx1])
                                  + rbp[indx1] * pmwt[indx1]); // this is R+B, interpolated
          }
#endif

        for(rr = 12; rr < rr1 - 12; rr++)
          for(cc = 12 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, indx1 = indx >> 1; cc < cc1 - 12;
              cc += 2, indx += 2, indx1++)
          {

            if(fabsf(0.5f - pmwt[indx >> 1]) < fabsf(0.5f - hvwt[indx >> 1])) continue;

            // now interpolate G vertically/horizontally using R+B values
            // unfortunately, since G interpolation cannot be done diagonally this may lead to color shifts
            // color ratios for G interpolation

            cru = cfa[indx - v1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 - v1)]);
            crd = cfa[indx + v1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 + v1)]);
            crl = cfa[indx - 1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 - 1)]);
            crr = cfa[indx + 1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 + 1)]);

            // interpolated G via adaptive ratios or Hamilton-Adams in each cardinal direction
            if(fabsf(1.0f - cru) < arthresh)
            {
              gu = rbint[indx1] * cru;
            }
            else
            {
              gu = cfa[indx - v1] + xdiv2f(rbint[indx1] - rbint[(indx1 - v1)]);
            }
            if(fabsf(1.0f - crd) < arthresh)
            {
              gd = rbint[indx1] * crd;
            }
            else
            {
              gd = cfa[indx + v1] + xdiv2f(rbint[indx1] - rbint[(indx1 + v1)]);
            }
            if(fabsf(1.0f - crl) < arthresh)
            {
              gl = rbint[indx1] * crl;
            }
            else
            {
              gl = cfa[indx - 1] + xdiv2f(rbint[indx1] - rbint[(indx1 - 1)]);
            }
            if(fabsf(1.0f - crr) < arthresh)
            {
              gr = rbint[indx1] * crr;
            }
            else
            {
              gr = cfa[indx + 1] + xdiv2f(rbint[indx1] - rbint[(indx1 + 1)]);
            }

            // gu=rbint[indx]*cru;
            // gd=rbint[indx]*crd;
            // gl=rbint[indx]*crl;
            // gr=rbint[indx]*crr;

            // interpolated G via adaptive weights of cardinal evaluations
            Gintv = (dirwts0[indx - v1] * gd + dirwts0[indx + v1] * gu)
                    / (dirwts0[indx + v1] + dirwts0[indx - v1]);
            Ginth = (dirwts1[indx - 1] * gr + dirwts1[indx + 1] * gl)
                    / (dirwts1[indx - 1] + dirwts1[indx + 1]);

            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            // bound the interpolation in regions of high saturation
            if(Gintv < rbint[indx1])
            {
              if(2 * Gintv < rbint[indx1])
              {
                Gintv = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
              }
              else
              {
                vwt = 2.0 * (rbint[indx1] - Gintv) / (eps + Gintv + rbint[indx1]);
                Gintv = vwt * Gintv + (1.0f - vwt) * ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
              }
            }
            if(Ginth < rbint[indx1])
            {
              if(2 * Ginth < rbint[indx1])
              {
                Ginth = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]);
              }
              else
              {
                hwt = 2.0 * (rbint[indx1] - Ginth) / (eps + Ginth + rbint[indx1]);
                Ginth = hwt * Ginth + (1.0f - hwt) * ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]);
              }
            }

            if(Ginth > clip_pt) Ginth = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]); // for RT implementation
            if(Gintv > clip_pt) Gintv = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
            // c=FC(rr,cc,filters);//for dcraw implementation
            // if (Ginth > pre_mul[c]) Ginth=ULIM(Ginth,cfa[indx-1],cfa[indx+1]);
            // if (Gintv > pre_mul[c]) Gintv=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            rgbgreen[indx] = Ginth * (1.0f - hvwt[indx1]) + Gintv * hvwt[indx1];
            // rgb[indx][1] =
            // 0.5*(rgb[indx][1]+0.25*(rgb[indx-v1][1]+rgb[indx+v1][1]+rgb[indx-1][1]+rgb[indx+1][1]));
            Dgrb[0][indx >> 1] = rgbgreen[indx] - cfa[indx];

            // rgb[indx][2-FC(rr,cc,filters)]=2*rbint[indx]-cfa[indx];
          }
        // end of diagonal interpolation correction
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // fancy chrominance interpolation
        //(ey,ex) is location of R site
        for(rr = 13 - ey; rr < rr1 - 12; rr += 2)
          for(cc = 13 - ex, indx1 = (rr * TS + cc) >> 1; cc < cc1 - 12; cc += 2, indx1++) // B coset
          {
            Dgrb[1][indx1] = Dgrb[0][indx1]; // split out G-B from G-R
            Dgrb[0][indx1] = 0;
          }
#ifdef __SSE2__
        //			__m128 wtnwv,wtnev,wtswv,wtsev;
        __m128 oned325v = _mm_set1_ps(1.325f);
        __m128 zd175v = _mm_set1_ps(0.175f);
        __m128 zd075v = _mm_set1_ps(0.075f);
#endif
        for(rr = 14; rr < rr1 - 14; rr++)
#ifdef __SSE2__
          for(cc = 14 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, c = 1 - FC(rr, cc, filters) / 2;
              cc < cc1 - 14; cc += 8, indx += 8)
          {
            wtnwv = onev / (epsv + vabsf(LVFU(Dgrb[c][(indx - m1) >> 1]) - LVFU(Dgrb[c][(indx + m1) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx - m1) >> 1]) - LVFU(Dgrb[c][(indx - m3) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx + m1) >> 1]) - LVFU(Dgrb[c][(indx - m3) >> 1])));
            wtnev = onev / (epsv + vabsf(LVFU(Dgrb[c][(indx + p1) >> 1]) - LVFU(Dgrb[c][(indx - p1) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx + p1) >> 1]) - LVFU(Dgrb[c][(indx + p3) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx - p1) >> 1]) - LVFU(Dgrb[c][(indx + p3) >> 1])));
            wtswv = onev / (epsv + vabsf(LVFU(Dgrb[c][(indx - p1) >> 1]) - LVFU(Dgrb[c][(indx + p1) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx - p1) >> 1]) - LVFU(Dgrb[c][(indx + m3) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx + p1) >> 1]) - LVFU(Dgrb[c][(indx - p3) >> 1])));
            wtsev = onev / (epsv + vabsf(LVFU(Dgrb[c][(indx + m1) >> 1]) - LVFU(Dgrb[c][(indx - m1) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx + m1) >> 1]) - LVFU(Dgrb[c][(indx - p3) >> 1]))
                            + vabsf(LVFU(Dgrb[c][(indx - m1) >> 1]) - LVFU(Dgrb[c][(indx + m3) >> 1])));

            // Dgrb[indx][c]=(wtnw*Dgrb[indx-m1][c]+wtne*Dgrb[indx+p1][c]+wtsw*Dgrb[indx-p1][c]+wtse*Dgrb[indx+m1][c])/(wtnw+wtne+wtsw+wtse);

            _mm_storeu_ps(&Dgrb[c][indx >> 1], (wtnwv * (oned325v * LVFU(Dgrb[c][(indx - m1) >> 1])
                                                         - zd175v * LVFU(Dgrb[c][(indx - m3) >> 1])
                                                         - zd075v * LVFU(Dgrb[c][(indx - m1 - 2) >> 1])
                                                         - zd075v * LVFU(Dgrb[c][(indx - m1 - v2) >> 1]))
                                                + wtnev * (oned325v * LVFU(Dgrb[c][(indx + p1) >> 1])
                                                           - zd175v * LVFU(Dgrb[c][(indx + p3) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx + p1 + 2) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx + p1 + v2) >> 1]))
                                                + wtswv * (oned325v * LVFU(Dgrb[c][(indx - p1) >> 1])
                                                           - zd175v * LVFU(Dgrb[c][(indx - p3) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx - p1 - 2) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx - p1 - v2) >> 1]))
                                                + wtsev * (oned325v * LVFU(Dgrb[c][(indx + m1) >> 1])
                                                           - zd175v * LVFU(Dgrb[c][(indx + m3) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx + m1 + 2) >> 1])
                                                           - zd075v * LVFU(Dgrb[c][(indx + m1 + v2) >> 1])))
                                               / (wtnwv + wtnev + wtswv + wtsev));
          }

#else
          for(cc = 14 + (FC(rr, 2, filters) & 1), indx = rr * TS + cc, c = 1 - FC(rr, cc, filters) / 2;
              cc < cc1 - 14; cc += 2, indx += 2)
          {
            wtnw = 1.0f / (eps + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m1) >> 1])
                           + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx - m3) >> 1])
                           + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m3) >> 1]));
            wtne = 1.0f / (eps + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p1) >> 1])
                           + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx + p3) >> 1])
                           + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p3) >> 1]));
            wtsw = 1.0f / (eps + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p1) >> 1])
                           + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + m3) >> 1])
                           + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p3) >> 1]));
            wtse = 1.0f / (eps + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m1) >> 1])
                           + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - p3) >> 1])
                           + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m3) >> 1]));

            // Dgrb[indx][c]=(wtnw*Dgrb[indx-m1][c]+wtne*Dgrb[indx+p1][c]+wtsw*Dgrb[indx-p1][c]+wtse*Dgrb[indx+m1][c])/(wtnw+wtne+wtsw+wtse);

            Dgrb[c][indx >> 1]
                = (wtnw * (1.325f * Dgrb[c][(indx - m1) >> 1] - 0.175f * Dgrb[c][(indx - m3) >> 1]
                           - 0.075f * Dgrb[c][(indx - m1 - 2) >> 1] - 0.075f * Dgrb[c][(indx - m1 - v2) >> 1])
                   + wtne
                     * (1.325f * Dgrb[c][(indx + p1) >> 1] - 0.175f * Dgrb[c][(indx + p3) >> 1]
                        - 0.075f * Dgrb[c][(indx + p1 + 2) >> 1] - 0.075f * Dgrb[c][(indx + p1 + v2) >> 1])
                   + wtsw
                     * (1.325f * Dgrb[c][(indx - p1) >> 1] - 0.175f * Dgrb[c][(indx - p3) >> 1]
                        - 0.075f * Dgrb[c][(indx - p1 - 2) >> 1] - 0.075f * Dgrb[c][(indx - p1 - v2) >> 1])
                   + wtse * (1.325f * Dgrb[c][(indx + m1) >> 1] - 0.175f * Dgrb[c][(indx + m3) >> 1]
                             - 0.075f * Dgrb[c][(indx + m1 + 2) >> 1]
                             - 0.075f * Dgrb[c][(indx + m1 + v2) >> 1])) / (wtnw + wtne + wtsw + wtse);
          }
#endif
        float temp;
        for(rr = 16; rr < rr1 - 16; rr++)
        {
          if((FC(rr, 2, filters) & 1) == 1)
          {
            for(cc = 16, indx = rr * TS + cc, row = rr + top; cc < cc1 - 16 - (cc1 & 1); cc += 2, indx++)
            {
              col = cc + left;
              if(col < roi_out->width && row < roi_out->height)
              {
                temp = 1.0f / ((hvwt[(indx - v1) >> 1]) + (1.0f - hvwt[(indx + 1) >> 1])
                               + (1.0f - hvwt[(indx - 1) >> 1]) + (hvwt[(indx + v1) >> 1]));
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
              }

              indx++;
              col++;
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0f, 1.0f);
              }
            }
            if(cc1 & 1) // width of tile is odd
            {
              col = cc + left;
              if(col < roi_out->width && row < roi_out->height)
              {
                temp = 1.0f / ((hvwt[(indx - v1) >> 1]) + (1.0f - hvwt[(indx + 1) >> 1])
                               + (1.0f - hvwt[(indx - 1) >> 1]) + (hvwt[(indx + v1) >> 1]));
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
              }
            }
          }
          else
          {
            for(cc = 16, indx = rr * TS + cc, row = rr + top; cc < cc1 - 16 - (cc1 & 1); cc += 2, indx++)
            {
              col = cc + left;
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0f, 1.0f);
              }

              indx++;
              col++;
              if(col < roi_out->width && row < roi_out->height)
              {
                temp = 1.0f / ((hvwt[(indx - v1) >> 1]) + (1.0f - hvwt[(indx + 1) >> 1])
                               + (1.0f - hvwt[(indx - 1) >> 1]) + (hvwt[(indx + v1) >> 1]));
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx]
                               - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                  + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                  + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                  + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) * temp,
                               0.0f, 1.0f);
              }
            }
            if(cc1 & 1) // width of tile is odd
            {
              col = cc + left;
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0f, 1.0f);
              }
            }
          }
        }


        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // copy smoothed results back to image matrix
        for(rr = 16; rr < rr1 - 16; rr++)
        {
          // TODO (darktable): we have the pixel colors interleaved so writing them in blocks using SSE2 is
          // not possible. or is it?
          // #ifdef __SSE2__
          //           for (row=rr+top, cc=16; cc < cc1-19; cc+=4)
          //           {
          //             col = cc + left;
          //             if(col < roi_out->width && row < roi_out->height)
          //               _mm_storeu_ps(&out[(row*roi_out->width+col)*4+1], LVF(rgbgreen[rr*TS+cc]));
          //           }
          // #else
          for(row = rr + top, cc = 16; cc < cc1 - 16; cc++)
          {
            col = cc + left;
            indx = rr * TS + cc;
            if(col < roi_out->width && row < roi_out->height)
              out[(row * roi_out->width + col) * 4 + 1] = clampnan(rgbgreen[indx], 0.0f, 1.0f);

            // for dcraw implementation
            // for (c=0; c<3; c++){
            //	image[indx][c] = CLIP((int)(65535.0f*rgb[rr*TS+cc][c] + 0.5f));
            //}
          }
          // #endif
        }
        // end of main loop

        //         if(plistener)
        //         {
        //           progresscounter++;
        //           if(progresscounter % 4 == 0)
        //           {
        // #ifdef _OPENMP
        //             #pragma omp critical
        // #endif
        //             {
        //               progress+=(double)4*((TS-32)*(TS-32))/(height*width);
        //               if (progress>1.0)
        //               {
        //                 progress=1.0;
        //               }
        //               plistener->setProgress(progress);
        //             }
        //           }
        //         }
      }

    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%



    // clean up
    free(buffer);
  }
//   if(plistener)
//     plistener->setProgress(1.0);


// done

#ifdef __SSE2__
#undef ZEROV
#undef LVF
#undef LVFU
#undef LC2VFU
#endif

#undef TS
#undef TSH
}
// }

/*==================================================================================
 * end of raw therapee code
 *==================================================================================*/

#undef SQR
#undef LIM
#undef ULIM
#undef HCLIP
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
