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
//#ifdef HAVE_CONFIG_H
//#include "config.h"
//#endif
//#include "common/darktable.h"
//#include "develop/imageop.h"
//#include "dtgtk/slider.h"
//#include "gui/gtk.h"
//#include <gtk/gtk.h>
//#include <stdlib.h>


#include <math.h>

static inline
float clampnan(const float x, const float m, const float M)
{
  float r;

  // clamp to [m, M] if x is infinite; return average of m and M if x is NaN; else just return x

  if(isinf(x))
    r = (isless(x, m) ? m : (isgreater(x, M) ? M : x));
  else if(isnan(x))
    r = (m + M)/2.0f;
  else // normal number
    r = x;

  return r;
}

/*==================================================================================
 * begin raw therapee code, hg checkout of june 04, 2013 branch master.
 *==================================================================================*/

__inline float xmul2f(float d)
{
  if (*(int*)&d & 0x7FFFFFFF)   // if f==0 do nothing
  {
    *(int*)&d += 1 << 23; // add 1 to the exponent
  }
  return d;
}

__inline float xdiv2f(float d)
{
  if (*(int*)&d & 0x7FFFFFFF)   // if f==0 do nothing
  {
    *(int*)&d -= 1 << 23; // sub 1 from the exponent
  }
  return d;
}

__inline float xdivf( float d, int n)
{
  if (*(int*)&d & 0x7FFFFFFF)   // if f==0 do nothing
  {
    *(int*)&d -= n << 23; // add n to the exponent
  }
  return d;
}

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

// using namespace rtengine;

// void RawImageSource::amaze_demosaic_RT(int winx, int winy, int winw, int winh)
static void
amaze_demosaic_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const in, float *out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const int filters)
{
#define SQR(x) ((x)*(x))
  //#define MIN(a,b) ((a) < (b) ? (a) : (b))
  //#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))
#define ULIM(x,y,z) ((y) < (z) ? LIM(x,y,z) : LIM(x,z,y))
  //#define CLIP(x) LIM(x,0,65535)
#define HCLIP(x) x //is this still necessary???
  //MIN(clip_pt,x)

  int winx = roi_out->x;
  int winy = roi_out->y;
  int winw = roi_in->width;
  int winh = roi_in->height;
  int width=winw, height=winh;

  //const uint32_t filters = dt_image_flipped_filter(self->dev->image);

  //const float clip_pt = 1/initialGain;
  const float clip_pt = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));


#define TS 512	 // Tile size; the image is processed in square tiles to lower memory requirements and facilitate multi-threading
#define TSH	256
#define TS6 500
  // local variables


  //offset of R pixel within a Bayer quartet
  int ex, ey;

  //shifts of pointer value to access pixels in vertical and diagonal directions
  static const int v1=TS, v2=2*TS, v3=3*TS, p1=-TS+1, p2=-2*TS+2, p3=-3*TS+3, m1=TS+1, m2=2*TS+2, m3=3*TS+3;

  //tolerance to avoid dividing by zero
  static const float eps=1e-5, epssq=1e-10;			//tolerance to avoid dividing by zero

  //adaptive ratios threshold
  static const float arthresh=0.75;
  //nyquist texture test threshold
  static const float nyqthresh=0.5;

  //gaussian on 5x5 quincunx, sigma=1.2
  static const float gaussodd[4] = {0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f, 0.0365543548389495f};
  //gaussian on 5x5, sigma=1.2
  static const float gaussgrad[6] = {0.07384411893421103f, 0.06207511968171489f, 0.0521818194747806f,
                                     0.03687419286733595f, 0.03099732204057846f, 0.018413194161458882f
                                    };
  //gaussian on 5x5 alt quincunx, sigma=1.5
  static const float gausseven[2] = {0.13719494435797422f, 0.05640252782101291f};
  //guassian on quincunx grid
  static const float gquinc[4] = {0.169917f, 0.108947f, 0.069855f, 0.0287182f};

  // volatile double progress = 0.0;
  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%



// Issue 1676

// Moved from inside the parallel section

  //if (plistener) {
  //	plistener->setProgressStr (Glib::ustring::compose(M("TP_RAW_DMETHOD_PROGRESSBAR"), RAWParams::methodstring[RAWParams::amaze]));
  //	plistener->setProgress (0.0);
  //}
  typedef struct s_mp
  {
    float m;
    float p;
  } s_mp;
  typedef struct s_hv
  {
    float h;
    float v;
  } s_hv;
#ifdef _OPENMP
  #pragma omp parallel
#endif
  {
    //position of top/left corner of the tile
    int top, left;
    // beginning of storage block for tile
    char  *buffer;
    // rgb values
    float (*rgb)[3];
    // horizontal gradient
    float (*delh);
    // vertical gradient
    float (*delv);
    // square of delh
    float (*delhsq);
    // square of delv
    float (*delvsq);
    // gradient based directional weights for interpolation
    float (*dirwts)[2];
    // vertically interpolated color differences G-R, G-B
    float (*vcd);
    // horizontally interpolated color differences
    float (*hcd);
    // alternative vertical interpolation
    float (*vcdalt);
    // alternative horizontal interpolation
    float (*hcdalt);
    // square of average color difference
    float (*cddiffsq);
    // weight to give horizontal vs vertical interpolation
    float (*hvwt);
    // final interpolated color difference
    float (*Dgrb)[2];
    // gradient in plus (NE/SW) direction
    float (*delp);
    // gradient in minus (NW/SE) direction
    float (*delm);
    // diagonal interpolation of R+B
    float (*rbint);
    s_hv  (*Dgrb2);
    // horizontal curvature of interpolated G (used to refine interpolation in Nyquist texture regions)
//	float (*Dgrbh2);
    // vertical curvature of interpolated G
//	float (*Dgrbv2);
    // difference between up/down interpolations of G
    float (*dgintv);
    // difference between left/right interpolations of G
    float (*dginth);
    // diagonal (plus) color difference R-B or G1-G2
//	float (*Dgrbp1);
    // diagonal (minus) color difference R-B or G1-G2
//	float (*Dgrbm1);
    s_mp  (*Dgrbsq1);
    // square of diagonal color difference
//	float (*Dgrbpsq1);
    // square of diagonal color difference
//	float (*Dgrbmsq1);
    // tile raw data
    float (*cfa);
    // relative weight for combining plus and minus diagonal interpolations
    float (*pmwt);
    // interpolated color difference R-B in minus and plus direction
    s_mp  (*rb);
    // interpolated color difference R-B in plus direction
//	float (*rbp);
    // interpolated color difference R-B in minus direction
//	float (*rbm);

    // nyquist texture flag 1=nyquist, 0=not nyquist
    char   (*nyquist);

#define CLF 1
    // assign working space
    buffer = (char *) malloc(29*sizeof(float)*TS*TS - sizeof(float)*TS*TSH + sizeof(char)*TS*TSH+23*CLF*64);
    char 	*data;
    data = (char *)( ((uintptr_t)buffer + 63) / 64 * 64);

    //merror(buffer,"amaze_interpolate()");
    //memset(buffer,0,(34*sizeof(float)+sizeof(int))*TS*TS);
    // rgb array
    rgb			= (float (*)[3])		data; //pointers to array
    delh		= (float (*))			(data +  3*sizeof(float)*TS*TS+1*CLF*64);
    delv		= (float (*))			(data +  4*sizeof(float)*TS*TS+2*CLF*64);
    delhsq		= (float (*))			(data +  5*sizeof(float)*TS*TS+3*CLF*64);
    delvsq		= (float (*))			(data +  6*sizeof(float)*TS*TS+4*CLF*64);
    dirwts		= (float (*)[2])		(data +  7*sizeof(float)*TS*TS+5*CLF*64);
    vcd			= (float (*))			(data +  9*sizeof(float)*TS*TS+6*CLF*64);
    hcd			= (float (*))			(data +  10*sizeof(float)*TS*TS+7*CLF*64);
    vcdalt		= (float (*))			(data +  11*sizeof(float)*TS*TS+8*CLF*64);
    hcdalt		= (float (*))			(data +  12*sizeof(float)*TS*TS+9*CLF*64);
    cddiffsq	= (float (*))			(data +  13*sizeof(float)*TS*TS+10*CLF*64);
    hvwt		= (float (*))			(data +  14*sizeof(float)*TS*TS+11*CLF*64);							//compressed			0.5 MB
    Dgrb		= (float (*)[2])		(data +  15*sizeof(float)*TS*TS - sizeof(float)*TS*TSH+12*CLF*64);
    delp		= (float (*))			(data +  17*sizeof(float)*TS*TS - sizeof(float)*TS*TSH+13*CLF*64);	// compressed			0.5 MB
    delm		= (float (*))			(data +  17*sizeof(float)*TS*TS+14*CLF*64);							// compressed			0.5 MB
    rbint		= (float (*))			(data +  18*sizeof(float)*TS*TS - sizeof(float)*TS*TSH+15*CLF*64);	// compressed			0.5 MB
    Dgrb2		= (s_hv  (*))			(data +  18*sizeof(float)*TS*TS+16*CLF*64);							// compressed			1.0 MB
//	Dgrbh2		= (float (*))			(data +  19*sizeof(float)*TS*TS);
//	Dgrbv2		= (float (*))			(data +  20*sizeof(float)*TS*TS);
    dgintv		= (float (*))			(data +  19*sizeof(float)*TS*TS+17*CLF*64);
    dginth		= (float (*))			(data +  20*sizeof(float)*TS*TS+18*CLF*64);
//	Dgrbp1		= (float (*))			(data +  23*sizeof(float)*TS*TS);													1.0 MB
//	Dgrbm1		= (float (*))			(data +  23*sizeof(float)*TS*TS);													1.0 MB
    Dgrbsq1		= (s_mp  (*))			(data +  21*sizeof(float)*TS*TS+19*CLF*64);							// compressed			1.0 MB
//	Dgrbpsq1	= (float (*))			(data +  23*sizeof(float)*TS*TS);
//	Dgrbmsq1	= (float (*))			(data +  24*sizeof(float)*TS*TS);
    cfa			= (float (*))			(data +  22*sizeof(float)*TS*TS+20*CLF*64);
    pmwt		= (float (*))			(data +  23*sizeof(float)*TS*TS+21*CLF*64);		// compressed								0.5 MB
    rb			= (s_mp  (*))			(data +  24*sizeof(float)*TS*TS - sizeof(float)*TS*TSH+22*CLF*64);		// compressed		1.0 MB
//	rbp			= (float (*))			(data +  30*sizeof(float)*TS*TS);
//	rbm			= (float (*))			(data +  31*sizeof(float)*TS*TS);

    nyquist		= (char (*))				(data +  25*sizeof(float)*TS*TS - sizeof(float)*TS*TSH+23*CLF*64);	//compressed		0.875 MB
#undef CLF
    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

    /*double dt;
     clock_t t1, t2;

     clock_t t1_init,       t2_init       = 0;
     clock_t t1_vcdhcd,      t2_vcdhcd      = 0;
     clock_t t1_cdvar,		t2_cdvar = 0;
     clock_t t1_nyqtest,   t2_nyqtest   = 0;
     clock_t t1_areainterp,  t2_areainterp  = 0;
     clock_t t1_compare,   t2_compare   = 0;
     clock_t t1_diag,   t2_diag   = 0;
     clock_t t1_chroma,    t2_chroma    = 0;*/


    // start
    //if (verbose) fprintf (stderr,_("AMaZE interpolation ...\n"));
    //t1 = clock();

    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


    //determine GRBG coset; (ey,ex) is the offset of the R subarray
    if (FC(0,0,filters)==1)  //first pixel is G
    {
      if (FC(0,1,filters)==0)
      {
        ey=0;
        ex=1;
      }
      else
      {
        ey=1;
        ex=0;
      }
    }
    else    //first pixel is R or B
    {
      if (FC(0,0,filters)==0)
      {
        ey=0;
        ex=0;
      }
      else
      {
        ey=1;
        ex=1;
      }
    }

    // Main algorithm: Tile loop
    //#ifdef _OPENMP
    //#pragma omp parallel for shared(rawData,height,width,red,green,blue) private(top,left) schedule(dynamic)
    //#endif
    //code is openmp ready; just have to pull local tile variable declarations inside the tile loop



// Issue 1676

// use collapse(2) to collapse the 2 loops to one large loop, so there is better scaling
// WARNING: we don't use collapse(2) as this seems to trigger an issue in some versions of gcc 4.8

#ifdef _OPENMP
    #pragma omp for schedule(dynamic) nowait
#endif
    for (top=winy-16; top < winy+height; top += TS-32)
      for (left=winx-16; left < winx+width; left += TS-32)
      {
        memset(nyquist, 0, sizeof(char)*TS*TSH);
        memset(rbint, 0, sizeof(float)*TS*TSH);
        //location of tile bottom edge
        int bottom = MIN(top+TS,winy+height+16);
        //location of tile right edge
        int right  = MIN(left+TS,winx+width+16);
        //tile width  (=TS except for right edge of image)
        int rr1 = bottom - top;
        //tile height (=TS except for bottom edge of image)
        int cc1 = right - left;

        //tile vars
        //counters for pixel location in the image
        int row, col;
        //min and max row/column in the tile
        int rrmin, rrmax, ccmin, ccmax;
        //counters for pixel location within the tile
        int rr, cc;
        //color index 0=R, 1=G, 2=B
        int c;
        //pointer counters within the tile
        int indx, indx1;
        //dummy indices
        int i, j;
        // +1 or -1
//			int sgn;

        //color ratios in up/down/left/right directions
        float cru, crd, crl, crr;
        //adaptive weights for vertical/horizontal/plus/minus directions
        float vwt, hwt, pwt, mwt;
        //vertical and horizontal G interpolations
        float Gintv, Ginth;
        //G interpolated in vert/hor directions using adaptive ratios
        float guar, gdar, glar, grar;
        //G interpolated in vert/hor directions using Hamilton-Adams method
        float guha, gdha, glha, grha;
        //interpolated G from fusing left/right or up/down
        float Ginthar, Ginthha, Gintvar, Gintvha;
        //color difference (G-R or G-B) variance in up/down/left/right directions
        float Dgrbvvaru, Dgrbvvard, Dgrbhvarl, Dgrbhvarr;
        float uave, dave, lave, rave;

        //color difference variances in vertical and horizontal directions
        float vcdvar, hcdvar, vcdvar1, hcdvar1, hcdaltvar, vcdaltvar;
        //adaptive interpolation weight using variance of color differences
        float varwt;																										// 639 - 644
        //adaptive interpolation weight using difference of left-right and up-down G interpolations
        float diffwt;																										// 640 - 644
        //alternative adaptive weight for combining horizontal/vertical interpolations
        float hvwtalt;																										// 745 - 748
        //temporary variables for combining interpolation weights at R and B sites
//			float vo, ve;
        //interpolation of G in four directions
        float gu, gd, gl, gr;
        //variance of G in vertical/horizontal directions
        float gvarh, gvarv;

        //Nyquist texture test
        float nyqtest;																										// 658 - 681
        //accumulators for Nyquist texture interpolation
        float sumh, sumv, sumsqh, sumsqv, areawt;

        //color ratios in diagonal directions
        float crse, crnw, crne, crsw;
        //color differences in diagonal directions
        float rbse, rbnw, rbne, rbsw;
        //adaptive weights for combining diagonal interpolations
        float wtse, wtnw, wtsw, wtne;
        //alternate weight for combining diagonal interpolations
        float pmwtalt;																										// 885 - 888
        //variance of R-B in plus/minus directions
        float rbvarm;																										// 843 - 848



        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


        // rgb from input CFA data
        // rgb values should be floating point number between 0 and 1
        // after white balance multipliers are applied
        // a 16 pixel border is added to each side of the image

        // bookkeeping for borders
        if (top<winy)
        {
          rrmin=16;
        }
        else
        {
          rrmin=0;
        }
        if (left<winx)
        {
          ccmin=16;
        }
        else
        {
          ccmin=0;
        }
        if (bottom>(winy+height))
        {
          rrmax=winy+height-top;
        }
        else
        {
          rrmax=rr1;
        }
        if (right>(winx+width))
        {
          ccmax=winx+width-left;
        }
        else
        {
          ccmax=cc1;
        }

        for (rr=rrmin; rr < rrmax; rr++)
          for (row=rr+top, cc=ccmin; cc < ccmax; cc++)
          {
            col = cc+left;
            c = FC(rr,cc,filters);
            indx1=rr*TS+cc;
            rgb[indx1][c] = (in[row*width + col]);
            //indx=row*width+col;
            //rgb[indx1][c] = image[indx][c]/65535.0f;//for dcraw implementation

            cfa[indx1] = rgb[indx1][c];
          }
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        //fill borders
        if (rrmin>0)
        {
          for (rr=0; rr<16; rr++)
            for (cc=ccmin; cc<ccmax; cc++)
            {
              c = FC(rr,cc,filters);
              rgb[rr*TS+cc][c] = rgb[(32-rr)*TS+cc][c];
              cfa[rr*TS+cc] = rgb[rr*TS+cc][c];
            }
        }
        if (rrmax<rr1)
        {
          for (rr=0; rr<16; rr++)
            for (cc=ccmin; cc<ccmax; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[(rrmax+rr)*TS+cc][c] = in[(winy+height-rr-2)*width+left+cc];
              //rgb[(rrmax+rr)*TS+cc][c] = (image[(height-rr-2)*width+left+cc][c])/65535.0f;//for dcraw implementation
              cfa[(rrmax+rr)*TS+cc] = rgb[(rrmax+rr)*TS+cc][c];
            }
        }
        if (ccmin>0)
        {
          for (rr=rrmin; rr<rrmax; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[rr*TS+cc][c] = rgb[rr*TS+32-cc][c];
              cfa[rr*TS+cc] = rgb[rr*TS+cc][c];
            }
        }
        if (ccmax<cc1)
        {
          for (rr=rrmin; rr<rrmax; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[rr*TS+ccmax+cc][c] = in[(top+rr)*width + (winx+width-cc-2)];
              //rgb[rr*TS+ccmax+cc][c] = (image[(top+rr)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
              cfa[rr*TS+ccmax+cc] = rgb[rr*TS+ccmax+cc][c];
            }
        }

        //also, fill the image corners
        if (rrmin>0 && ccmin>0)
        {
          for (rr=0; rr<16; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[(rr)*TS+cc][c] = in[(winy+32-rr)*width + winx+32-cc];
              //rgb[(rr)*TS+cc][c] = (rgb[(32-rr)*TS+(32-cc)][c]);//for dcraw implementation
              cfa[(rr)*TS+cc] = rgb[(rr)*TS+cc][c];
            }
        }
        if (rrmax<rr1 && ccmax<cc1)
        {
          for (rr=0; rr<16; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[(rrmax+rr)*TS+ccmax+cc][c] = in[(winy+height-rr-2)*width + (winx+width-cc-2)];
              //rgb[(rrmax+rr)*TS+ccmax+cc][c] = (image[(height-rr-2)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
              cfa[(rrmax+rr)*TS+ccmax+cc] = rgb[(rrmax+rr)*TS+ccmax+cc][c];
            }
        }
        if (rrmin>0 && ccmax<cc1)
        {
          for (rr=0; rr<16; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[(rr)*TS+ccmax+cc][c] = in[(winy+32-rr)*width + (winx+width-cc-2)];
              //rgb[(rr)*TS+ccmax+cc][c] = (image[(32-rr)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
              cfa[(rr)*TS+ccmax+cc] = rgb[(rr)*TS+ccmax+cc][c];
            }
        }
        if (rrmax<rr1 && ccmin>0)
        {
          for (rr=0; rr<16; rr++)
            for (cc=0; cc<16; cc++)
            {
              c=FC(rr,cc,filters);
              rgb[(rrmax+rr)*TS+cc][c] = in[(winy+height-rr-2)*width + (winx+32-cc)];
              //rgb[(rrmax+rr)*TS+cc][c] = (image[(height-rr-2)*width+(32-cc)][c])/65535.0f;//for dcraw implementation
              cfa[(rrmax+rr)*TS+cc] = rgb[(rrmax+rr)*TS+cc][c];
            }
        }

        //end of border fill
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        for (rr=1; rr < rr1-1; rr++)
          for (cc=1, indx=(rr)*TS+cc; cc < cc1-1; cc++, indx++)
          {

            delh[indx] = fabsf(cfa[indx+1]-cfa[indx-1]);
            delv[indx] = fabsf(cfa[indx+v1]-cfa[indx-v1]);
            delhsq[indx] = SQR(delh[indx]);
            delvsq[indx] = SQR(delv[indx]);
//					delp[indx] = fabsf(cfa[indx+p1]-cfa[indx-p1]);
//					delm[indx] = fabsf(cfa[indx+m1]-cfa[indx-m1]);
          }

        for (rr=2; rr < rr1-2; rr++)
          for (cc=2,indx=(rr)*TS+cc; cc < cc1-2; cc++, indx++)
          {
            dirwts[indx][0] = eps+delv[indx+v1]+delv[indx-v1]+delv[indx];//+fabsf(cfa[indx+v2]-cfa[indx-v2]);
            //vert directional averaging weights
            dirwts[indx][1] = eps+delh[indx+1]+delh[indx-1]+delh[indx];//+fabsf(cfa[indx+2]-cfa[indx-2]);
            //horizontal weights

          }

        for (rr=6; rr < rr1-6; rr++)
          for (cc=6+(FC(rr,2,filters)&1), indx=(rr)*TS+cc; cc < cc1-6; cc+=2, indx+=2)
          {
            delp[indx>>1] = fabsf(cfa[indx+p1]-cfa[indx-p1]);
            delm[indx>>1] = fabsf(cfa[indx+m1]-cfa[indx-m1]);
          }

        for (rr=6; rr < rr1-6; rr++)
          for (cc=6+(FC(rr,1,filters)&1),indx=(rr)*TS+cc; cc < cc1-6; cc+=2, indx+=2)
          {
            Dgrbsq1[indx>>1].p=(SQR(cfa[indx]-cfa[indx-p1])+SQR(cfa[indx]-cfa[indx+p1]));
            Dgrbsq1[indx>>1].m=(SQR(cfa[indx]-cfa[indx-m1])+SQR(cfa[indx]-cfa[indx+m1]));
          }



        //t2_init += clock()-t1_init;
        // end of tile initialization
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        //interpolate vertical and horizontal color differences
        //t1_vcdhcd = clock();

        for (rr=4; rr<rr1-4; rr++)
          //for (cc=4+(FC(rr,2,filters)&1),indx=rr*TS+cc,c=FC(rr,cc,filters); cc<cc1-4; cc+=2,indx+=2) {
          for (cc=4,indx=rr*TS+cc; cc<cc1-4; cc++,indx++)
          {
//					c=FC(rr,cc,filters);
//					if (c&1) {sgn=-1;} else {sgn=1;}

            //initialization of nyquist test
//					nyquist[indx]=0;
            //preparation for diag interp
//					rbint[indx]=0;

            //color ratios in each cardinal direction
            cru = cfa[indx-v1]*(dirwts[indx-v2][0]+dirwts[indx][0])/(dirwts[indx-v2][0]*(eps+cfa[indx])+dirwts[indx][0]*(eps+cfa[indx-v2]));
            crd = cfa[indx+v1]*(dirwts[indx+v2][0]+dirwts[indx][0])/(dirwts[indx+v2][0]*(eps+cfa[indx])+dirwts[indx][0]*(eps+cfa[indx+v2]));
            crl = cfa[indx-1]*(dirwts[indx-2][1]+dirwts[indx][1])/(dirwts[indx-2][1]*(eps+cfa[indx])+dirwts[indx][1]*(eps+cfa[indx-2]));
            crr = cfa[indx+1]*(dirwts[indx+2][1]+dirwts[indx][1])/(dirwts[indx+2][1]*(eps+cfa[indx])+dirwts[indx][1]*(eps+cfa[indx+2]));

            guha=HCLIP(cfa[indx-v1])+xdiv2f(cfa[indx]-cfa[indx-v2]);
            gdha=HCLIP(cfa[indx+v1])+xdiv2f(cfa[indx]-cfa[indx+v2]);
            glha=HCLIP(cfa[indx-1])+xdiv2f(cfa[indx]-cfa[indx-2]);
            grha=HCLIP(cfa[indx+1])+xdiv2f(cfa[indx]-cfa[indx+2]);
            /*
            					guha=HCLIP(cfa[indx-v1])+0.5*(cfa[indx]-cfa[indx-v2]);
            					gdha=HCLIP(cfa[indx+v1])+0.5*(cfa[indx]-cfa[indx+v2]);
            					glha=HCLIP(cfa[indx-1])+0.5*(cfa[indx]-cfa[indx-2]);
            					grha=HCLIP(cfa[indx+1])+0.5*(cfa[indx]-cfa[indx+2]);
            */
            if (fabsf(1.0f-cru)<arthresh)
            {
              guar=cfa[indx]*cru;
            }
            else
            {
              guar=guha;
            }
            if (fabsf(1.0f-crd)<arthresh)
            {
              gdar=cfa[indx]*crd;
            }
            else
            {
              gdar=gdha;
            }
            if (fabsf(1.0f-crl)<arthresh)
            {
              glar=cfa[indx]*crl;
            }
            else
            {
              glar=glha;
            }
            if (fabsf(1.0f-crr)<arthresh)
            {
              grar=cfa[indx]*crr;
            }
            else
            {
              grar=grha;
            }

            hwt = dirwts[indx-1][1]/(dirwts[indx-1][1]+dirwts[indx+1][1]);
            vwt = dirwts[indx-v1][0]/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);

            //interpolated G via adaptive weights of cardinal evaluations
            Gintvar = vwt*gdar+(1.0f-vwt)*guar;
            Ginthar = hwt*grar+(1.0f-hwt)*glar;
            Gintvha = vwt*gdha+(1.0f-vwt)*guha;
            Ginthha = hwt*grha+(1.0f-hwt)*glha;
            //interpolated color differences
            if (FC(rr,cc,filters)&1)
            {
              vcd[indx] = cfa[indx]-Gintvar;
              hcd[indx] = cfa[indx]-Ginthar;
              vcdalt[indx] = cfa[indx]-Gintvha;
              hcdalt[indx] = cfa[indx]-Ginthha;
            }
            else
            {
              //interpolated color differences
              vcd[indx] = Gintvar-cfa[indx];
              hcd[indx] = Ginthar-cfa[indx];
              vcdalt[indx] = Gintvha-cfa[indx];
              hcdalt[indx] = Ginthha-cfa[indx];
            }
            /*
            					vcd[indx] = sgn*(Gintvar-cfa[indx]);
            					hcd[indx] = sgn*(Ginthar-cfa[indx]);
            					vcdalt[indx] = sgn*(Gintvha-cfa[indx]);
            					hcdalt[indx] = sgn*(Ginthha-cfa[indx]);
            */
            if (cfa[indx] > 0.8*clip_pt || Gintvha > 0.8*clip_pt || Ginthha > 0.8*clip_pt)
            {
              //use HA if highlights are (nearly) clipped
              guar=guha;
              gdar=gdha;
              glar=glha;
              grar=grha;
              vcd[indx]=vcdalt[indx];
              hcd[indx]=hcdalt[indx];
            }

            //differences of interpolations in opposite directions
            dgintv[indx]=MIN(SQR(guha-gdha),SQR(guar-gdar));
            dginth[indx]=MIN(SQR(glha-grha),SQR(glar-grar));

          }
        //t2_vcdhcd += clock() - t1_vcdhcd;

        //t1_cdvar = clock();
        for (rr=4; rr<rr1-4; rr++)
          //for (cc=4+(FC(rr,2,filters)&1),indx=rr*TS+cc,c=FC(rr,cc,filters); cc<cc1-4; cc+=2,indx+=2) {
          for (cc=4,indx=rr*TS+cc; cc<cc1-4; cc++,indx++)
          {
            c=FC(rr,cc,filters);

            hcdvar =3.0f*(SQR(hcd[indx-2])+SQR(hcd[indx])+SQR(hcd[indx+2]))-SQR(hcd[indx-2]+hcd[indx]+hcd[indx+2]);
            hcdaltvar =3.0f*(SQR(hcdalt[indx-2])+SQR(hcdalt[indx])+SQR(hcdalt[indx+2]))-SQR(hcdalt[indx-2]+hcdalt[indx]+hcdalt[indx+2]);
            vcdvar =3.0f*(SQR(vcd[indx-v2])+SQR(vcd[indx])+SQR(vcd[indx+v2]))-SQR(vcd[indx-v2]+vcd[indx]+vcd[indx+v2]);
            vcdaltvar =3.0f*(SQR(vcdalt[indx-v2])+SQR(vcdalt[indx])+SQR(vcdalt[indx+v2]))-SQR(vcdalt[indx-v2]+vcdalt[indx]+vcdalt[indx+v2]);
            //choose the smallest variance; this yields a smoother interpolation
            if (hcdaltvar<hcdvar) hcd[indx]=hcdalt[indx];
            if (vcdaltvar<vcdvar) vcd[indx]=vcdalt[indx];

            //bound the interpolation in regions of high saturation
            if (c&1)  //G site
            {
              Ginth = -hcd[indx]+cfa[indx];//R or B
              Gintv = -vcd[indx]+cfa[indx];//B or R

              if (hcd[indx]>0)
              {
                if (3.0f*hcd[indx] > (Ginth+cfa[indx]))
                {
                  hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];
                }
                else
                {
                  hwt = 1.0f -3.0f*hcd[indx]/(eps+Ginth+cfa[indx]);
                  hcd[indx]=hwt*hcd[indx] + (1.0f-hwt)*(-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx]);
                }
              }
              if (vcd[indx]>0)
              {
                if (3.0f*vcd[indx] > (Gintv+cfa[indx]))
                {
                  vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];
                }
                else
                {
                  vwt = 1.0f -3.0f*vcd[indx]/(eps+Gintv+cfa[indx]);
                  vcd[indx]=vwt*vcd[indx] + (1.0f-vwt)*(-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx]);
                }
              }

              if (Ginth > clip_pt) hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];//for RT implementation
              if (Gintv > clip_pt) vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];
              //if (Ginth > pre_mul[c]) hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];//for dcraw implementation
              //if (Gintv > pre_mul[c]) vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];

            }
            else    //R or B site
            {

              Ginth = hcd[indx]+cfa[indx];//interpolated G
              Gintv = vcd[indx]+cfa[indx];

              if (hcd[indx]<0)
              {
                if (3.0f*hcd[indx] < -(Ginth+cfa[indx]))
                {
                  hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];
                }
                else
                {
                  hwt = 1.0f +3.0f*hcd[indx]/(eps+Ginth+cfa[indx]);
                  hcd[indx]=hwt*hcd[indx] + (1.0f-hwt)*(ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx]);
                }
              }
              if (vcd[indx]<0)
              {
                if (3.0f*vcd[indx] < -(Gintv+cfa[indx]))
                {
                  vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
                }
                else
                {
                  vwt = 1.0f +3.0f*vcd[indx]/(eps+Gintv+cfa[indx]);
                  vcd[indx]=vwt*vcd[indx] + (1.0f-vwt)*(ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx]);
                }
              }

              if (Ginth > clip_pt) hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];//for RT implementation
              if (Gintv > clip_pt) vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
              //if (Ginth > pre_mul[c]) hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];//for dcraw implementation
              //if (Gintv > pre_mul[c]) vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
              cddiffsq[indx] = SQR(vcd[indx]-hcd[indx]);
            }

//					cddiffsq[indx] = SQR(vcd[indx]-hcd[indx]);
          }

        for (rr=6; rr<rr1-6; rr++)
          for (cc=6+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-6; cc+=2,indx+=2)
          {

            //compute color difference variances in cardinal directions

            uave = vcd[indx]+vcd[indx-v1]+vcd[indx-v2]+vcd[indx-v3];
            dave = vcd[indx]+vcd[indx+v1]+vcd[indx+v2]+vcd[indx+v3];
            lave = (hcd[indx]+hcd[indx-1]+hcd[indx-2]+hcd[indx-3]);
            rave = (hcd[indx]+hcd[indx+1]+hcd[indx+2]+hcd[indx+3]);

            Dgrbvvaru = SQR(vcd[indx]-uave)+SQR(vcd[indx-v1]-uave)+SQR(vcd[indx-v2]-uave)+SQR(vcd[indx-v3]-uave);
            Dgrbvvard = SQR(vcd[indx]-dave)+SQR(vcd[indx+v1]-dave)+SQR(vcd[indx+v2]-dave)+SQR(vcd[indx+v3]-dave);
            Dgrbhvarl = SQR(hcd[indx]-lave)+SQR(hcd[indx-1]-lave)+SQR(hcd[indx-2]-lave)+SQR(hcd[indx-3]-lave);
            Dgrbhvarr = SQR(hcd[indx]-rave)+SQR(hcd[indx+1]-rave)+SQR(hcd[indx+2]-rave)+SQR(hcd[indx+3]-rave);

            hwt = dirwts[indx-1][1]/(dirwts[indx-1][1]+dirwts[indx+1][1]);
            vwt = dirwts[indx-v1][0]/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);

            vcdvar = epssq+vwt*Dgrbvvard+(1.0f-vwt)*Dgrbvvaru;
            hcdvar = epssq+hwt*Dgrbhvarr+(1.0f-hwt)*Dgrbhvarl;

            //compute fluctuations in up/down and left/right interpolations of colors
            Dgrbvvaru = (dgintv[indx])+(dgintv[indx-v1])+(dgintv[indx-v2]);
            Dgrbvvard = (dgintv[indx])+(dgintv[indx+v1])+(dgintv[indx+v2]);
            Dgrbhvarl = (dginth[indx])+(dginth[indx-1])+(dginth[indx-2]);
            Dgrbhvarr = (dginth[indx])+(dginth[indx+1])+(dginth[indx+2]);

            vcdvar1 = epssq+vwt*Dgrbvvard+(1.0f-vwt)*Dgrbvvaru;
            hcdvar1 = epssq+hwt*Dgrbhvarr+(1.0f-hwt)*Dgrbhvarl;

            //determine adaptive weights for G interpolation
            varwt=hcdvar/(vcdvar+hcdvar);
            diffwt=hcdvar1/(vcdvar1+hcdvar1);

            //if both agree on interpolation direction, choose the one with strongest directional discrimination;
            //otherwise, choose the u/d and l/r difference fluctuation weights
            if ((0.5-varwt)*(0.5-diffwt)>0 && fabsf(0.5f-diffwt)<fabsf(0.5f-varwt))
            {
              hvwt[indx>>1]=varwt;
            }
            else
            {
              hvwt[indx>>1]=diffwt;
            }

            //hvwt[indx]=varwt;
          }
        //t2_cdvar += clock() - t1_cdvar;

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // Nyquist test
        //t1_nyqtest = clock();

        for (rr=6; rr<rr1-6; rr++)
          for (cc=6+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-6; cc+=2,indx+=2)
          {

            //nyquist texture test: ask if difference of vcd compared to hcd is larger or smaller than RGGB gradients
            nyqtest = (gaussodd[0]*cddiffsq[indx]+
                       gaussodd[1]*(cddiffsq[indx-m1]+cddiffsq[indx+p1]+
                                    cddiffsq[indx-p1]+cddiffsq[indx+m1])+
                       gaussodd[2]*(cddiffsq[indx-v2]+cddiffsq[indx-2]+
                                    cddiffsq[indx+2]+cddiffsq[indx+v2])+
                       gaussodd[3]*(cddiffsq[indx-m2]+cddiffsq[indx+p2]+
                                    cddiffsq[indx-p2]+cddiffsq[indx+m2]));

            nyqtest -= nyqthresh*(gaussgrad[0]*(delhsq[indx]+delvsq[indx])+
                                  gaussgrad[1]*(delhsq[indx-v1]+delvsq[indx-v1]+delhsq[indx+1]+delvsq[indx+1]+
                                                delhsq[indx-1]+delvsq[indx-1]+delhsq[indx+v1]+delvsq[indx+v1])+
                                  gaussgrad[2]*(delhsq[indx-m1]+delvsq[indx-m1]+delhsq[indx+p1]+delvsq[indx+p1]+
                                                delhsq[indx-p1]+delvsq[indx-p1]+delhsq[indx+m1]+delvsq[indx+m1])+
                                  gaussgrad[3]*(delhsq[indx-v2]+delvsq[indx-v2]+delhsq[indx-2]+delvsq[indx-2]+
                                                delhsq[indx+2]+delvsq[indx+2]+delhsq[indx+v2]+delvsq[indx+v2])+
                                  gaussgrad[4]*(delhsq[indx-2*TS-1]+delvsq[indx-2*TS-1]+delhsq[indx-2*TS+1]+delvsq[indx-2*TS+1]+
                                                delhsq[indx-TS-2]+delvsq[indx-TS-2]+delhsq[indx-TS+2]+delvsq[indx-TS+2]+
                                                delhsq[indx+TS-2]+delvsq[indx+TS-2]+delhsq[indx+TS+2]+delvsq[indx-TS+2]+
                                                delhsq[indx+2*TS-1]+delvsq[indx+2*TS-1]+delhsq[indx+2*TS+1]+delvsq[indx+2*TS+1])+
                                  gaussgrad[5]*(delhsq[indx-m2]+delvsq[indx-m2]+delhsq[indx+p2]+delvsq[indx+p2]+
                                                delhsq[indx-p2]+delvsq[indx-p2]+delhsq[indx+m2]+delvsq[indx+m2]));


            if (nyqtest>0)
            {
              nyquist[indx>>1]=1; //nyquist=1 for nyquist region
            }
          }
        unsigned int nyquisttemp;
        for (rr=8; rr<rr1-8; rr++)
          for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2)
          {

            nyquisttemp=(nyquist[(indx-v2)>>1]+nyquist[(indx-m1)>>1]+nyquist[(indx+p1)>>1]+
                         nyquist[(indx-2)>>1]+nyquist[indx>>1]+nyquist[(indx+2)>>1]+
                         nyquist[(indx-p1)>>1]+nyquist[(indx+m1)>>1]+nyquist[(indx+v2)>>1]);
            //if most of your neighbors are named Nyquist, it's likely that you're one too
            if (nyquisttemp>4) nyquist[indx>>1]=1;
            //or not
            if (nyquisttemp<4) nyquist[indx>>1]=0;
          }

        //t2_nyqtest += clock() - t1_nyqtest;
        // end of Nyquist test
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%




        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // in areas of Nyquist texture, do area interpolation
        //t1_areainterp = clock();
        for (rr=8; rr<rr1-8; rr++)
          for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2)
          {

            if (nyquist[indx>>1])
            {
              // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
              // area interpolation

              sumh=sumv=sumsqh=sumsqv=areawt=0;
              for (i=-6; i<7; i+=2)
                for (j=-6; j<7; j+=2)
                {
                  indx1=(rr+i)*TS+cc+j;
                  if (nyquist[indx1>>1])
                  {
                    sumh += cfa[indx1]-xdiv2f(cfa[indx1-1]+cfa[indx1+1]);
                    sumv += cfa[indx1]-xdiv2f(cfa[indx1-v1]+cfa[indx1+v1]);
                    sumsqh += xdiv2f(SQR(cfa[indx1]-cfa[indx1-1])+SQR(cfa[indx1]-cfa[indx1+1]));
                    sumsqv += xdiv2f(SQR(cfa[indx1]-cfa[indx1-v1])+SQR(cfa[indx1]-cfa[indx1+v1]));
                    areawt +=1;
                  }
                }

              //horizontal and vertical color differences, and adaptive weight
              hcdvar=epssq+fabsf(areawt*sumsqh-sumh*sumh);
              vcdvar=epssq+fabsf(areawt*sumsqv-sumv*sumv);
              hvwt[indx>>1]=hcdvar/(vcdvar+hcdvar);

              // end of area interpolation
              // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            }
          }
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        //t2_areainterp += clock() - t1_areainterp;

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        //populate G at R/B sites
        for (rr=8; rr<rr1-8; rr++)
          for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2)
          {

            //first ask if one gets more directional discrimination from nearby B/R sites
            hvwtalt = xdivf(hvwt[(indx-m1)>>1]+hvwt[(indx+p1)>>1]+hvwt[(indx-p1)>>1]+hvwt[(indx+m1)>>1],2);
//					hvwtalt = 0.25*(hvwt[(indx-m1)>>1]+hvwt[(indx+p1)>>1]+hvwt[(indx-p1)>>1]+hvwt[(indx+m1)>>1]);
//					vo=fabsf(0.5-hvwt[indx>>1]);
//					ve=fabsf(0.5-hvwtalt);
            if (fabsf(0.5f-hvwt[indx>>1])<fabsf(0.5f-hvwtalt))
            {
              hvwt[indx>>1]=hvwtalt; //a better result was obtained from the neighbors
            }
//					if (vo<ve) {hvwt[indx>>1]=hvwtalt;}//a better result was obtained from the neighbors



            Dgrb[indx][0] = (hcd[indx]*(1.0f-hvwt[indx>>1]) + vcd[indx]*hvwt[indx>>1]);//evaluate color differences
            //if (hvwt[indx]<0.5) Dgrb[indx][0]=hcd[indx];
            //if (hvwt[indx]>0.5) Dgrb[indx][0]=vcd[indx];
            rgb[indx][1] = cfa[indx] + Dgrb[indx][0];//evaluate G (finally!)

            //local curvature in G (preparation for nyquist refinement step)
            if (nyquist[indx>>1])
            {
              Dgrb2[indx>>1].h = SQR(rgb[indx][1] - xdiv2f(rgb[indx-1][1]+rgb[indx+1][1]));
              Dgrb2[indx>>1].v = SQR(rgb[indx][1] - xdiv2f(rgb[indx-v1][1]+rgb[indx+v1][1]));
            }
            else
            {
              Dgrb2[indx>>1].h = Dgrb2[indx>>1].v = 0;
            }
          }

        //end of standard interpolation
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // refine Nyquist areas using G curvatures

        for (rr=8; rr<rr1-8; rr++)
          for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2)
          {

            if (nyquist[indx>>1])
            {
              //local averages (over Nyquist pixels only) of G curvature squared
              gvarh = epssq + (gquinc[0]*Dgrb2[indx>>1].h+
                               gquinc[1]*(Dgrb2[(indx-m1)>>1].h+Dgrb2[(indx+p1)>>1].h+Dgrb2[(indx-p1)>>1].h+Dgrb2[(indx+m1)>>1].h)+
                               gquinc[2]*(Dgrb2[(indx-v2)>>1].h+Dgrb2[(indx-2)>>1].h+Dgrb2[(indx+2)>>1].h+Dgrb2[(indx+v2)>>1].h)+
                               gquinc[3]*(Dgrb2[(indx-m2)>>1].h+Dgrb2[(indx+p2)>>1].h+Dgrb2[(indx-p2)>>1].h+Dgrb2[(indx+m2)>>1].h));
              gvarv = epssq + (gquinc[0]*Dgrb2[indx>>1].v+
                               gquinc[1]*(Dgrb2[(indx-m1)>>1].v+Dgrb2[(indx+p1)>>1].v+Dgrb2[(indx-p1)>>1].v+Dgrb2[(indx+m1)>>1].v)+
                               gquinc[2]*(Dgrb2[(indx-v2)>>1].v+Dgrb2[(indx-2)>>1].v+Dgrb2[(indx+2)>>1].v+Dgrb2[(indx+v2)>>1].v)+
                               gquinc[3]*(Dgrb2[(indx-m2)>>1].v+Dgrb2[(indx+p2)>>1].v+Dgrb2[(indx-p2)>>1].v+Dgrb2[(indx+m2)>>1].v));
              //use the results as weights for refined G interpolation
              Dgrb[indx][0] = (hcd[indx]*gvarv + vcd[indx]*gvarh)/(gvarv+gvarh);
              rgb[indx][1] = cfa[indx] + Dgrb[indx][0];
            }
          }

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        //t1_diag = clock();
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // diagonal interpolation correction

        for (rr=8; rr<rr1-8; rr++)
          for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc,indx1=indx>>1; cc<cc1-8; cc+=2,indx+=2,indx1++)
          {

            /*
            					rbvarp = epssq + (gausseven[0]*(Dgrbsq1[indx-v1].p+Dgrbsq1[indx-1].p+Dgrbsq1[indx+1].p+Dgrbsq1[indx+v1].p) +
            									gausseven[1]*(Dgrbsq1[indx-v2-1].p+Dgrbsq1[indx-v2+1].p+Dgrbsq1[indx-2-v1].p+Dgrbsq1[indx+2-v1].p+
            												  Dgrbsq1[indx-2+v1].p+Dgrbsq1[indx+2+v1].p+Dgrbsq1[indx+v2-1].p+Dgrbsq1[indx+v2+1].p));
            					rbvarm = epssq + (gausseven[0]*(Dgrbsq1[indx-v1].m+Dgrbsq1[indx-1].m+Dgrbsq1[indx+1].m+Dgrbsq1[indx+v1].m) +
            									gausseven[1]*(Dgrbsq1[indx-v2-1].m+Dgrbsq1[indx-v2+1].m+Dgrbsq1[indx-2-v1].m+Dgrbsq1[indx+2-v1].m+
            												  Dgrbsq1[indx-2+v1].m+Dgrbsq1[indx+2+v1].m+Dgrbsq1[indx+v2-1].m+Dgrbsq1[indx+v2+1].m));
            */
            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            //diagonal color ratios
            crse=xmul2f(cfa[indx+m1])/(eps+cfa[indx]+(cfa[indx+m2]));
            crnw=xmul2f(cfa[indx-m1])/(eps+cfa[indx]+(cfa[indx-m2]));
            crne=xmul2f(cfa[indx+p1])/(eps+cfa[indx]+(cfa[indx+p2]));
            crsw=xmul2f(cfa[indx-p1])/(eps+cfa[indx]+(cfa[indx-p2]));

            //assign B/R at R/B sites
            if (fabsf(1.0f-crse)<arthresh)
            {
              rbse=cfa[indx]*crse; //use this if more precise diag interp is necessary
            }
            else
            {
              rbse=(cfa[indx+m1])+xdiv2f(cfa[indx]-cfa[indx+m2]);
            }
            if (fabsf(1.0f-crnw)<arthresh)
            {
              rbnw=cfa[indx]*crnw;
            }
            else
            {
              rbnw=(cfa[indx-m1])+xdiv2f(cfa[indx]-cfa[indx-m2]);
            }
            if (fabsf(1.0f-crne)<arthresh)
            {
              rbne=cfa[indx]*crne;
            }
            else
            {
              rbne=(cfa[indx+p1])+xdiv2f(cfa[indx]-cfa[indx+p2]);
            }
            if (fabsf(1.0f-crsw)<arthresh)
            {
              rbsw=cfa[indx]*crsw;
            }
            else
            {
              rbsw=(cfa[indx-p1])+xdiv2f(cfa[indx]-cfa[indx-p2]);
            }

            wtse= eps+delm[indx1]+delm[(indx+m1)>>1]+delm[(indx+m2)>>1];//same as for wtu,wtd,wtl,wtr
            wtnw= eps+delm[indx1]+delm[(indx-m1)>>1]+delm[(indx-m2)>>1];
            wtne= eps+delp[indx1]+delp[(indx+p1)>>1]+delp[(indx+p2)>>1];
            wtsw= eps+delp[indx1]+delp[(indx-p1)>>1]+delp[(indx-p2)>>1];


            rb[indx1].m = (wtse*rbnw+wtnw*rbse)/(wtse+wtnw);
            rb[indx1].p = (wtne*rbsw+wtsw*rbne)/(wtne+wtsw);
            /*
            					rbvarp = epssq + (gausseven[0]*(Dgrbsq1[indx-v1].p+Dgrbsq1[indx-1].p+Dgrbsq1[indx+1].p+Dgrbsq1[indx+v1].p) +
            									gausseven[1]*(Dgrbsq1[indx-v2-1].p+Dgrbsq1[indx-v2+1].p+Dgrbsq1[indx-2-v1].p+Dgrbsq1[indx+2-v1].p+
            												  Dgrbsq1[indx-2+v1].p+Dgrbsq1[indx+2+v1].p+Dgrbsq1[indx+v2-1].p+Dgrbsq1[indx+v2+1].p));
            */
            rbvarm = epssq + (gausseven[0]*(Dgrbsq1[(indx-v1)>>1].m+Dgrbsq1[(indx-1)>>1].m+Dgrbsq1[(indx+1)>>1].m+Dgrbsq1[(indx+v1)>>1].m) +
                              gausseven[1]*(Dgrbsq1[(indx-v2-1)>>1].m+Dgrbsq1[(indx-v2+1)>>1].m+Dgrbsq1[(indx-2-v1)>>1].m+Dgrbsq1[(indx+2-v1)>>1].m+
                                            Dgrbsq1[(indx-2+v1)>>1].m+Dgrbsq1[(indx+2+v1)>>1].m+Dgrbsq1[(indx+v2-1)>>1].m+Dgrbsq1[(indx+v2+1)>>1].m));
            pmwt[indx1] = rbvarm/((epssq + (gausseven[0]*(Dgrbsq1[(indx-v1)>>1].p+Dgrbsq1[(indx-1)>>1].p+Dgrbsq1[(indx+1)>>1].p+Dgrbsq1[(indx+v1)>>1].p) +
                                            gausseven[1]*(Dgrbsq1[(indx-v2-1)>>1].p+Dgrbsq1[(indx-v2+1)>>1].p+Dgrbsq1[(indx-2-v1)>>1].p+Dgrbsq1[(indx+2-v1)>>1].p+
                                                Dgrbsq1[(indx-2+v1)>>1].p+Dgrbsq1[(indx+2+v1)>>1].p+Dgrbsq1[(indx+v2-1)>>1].p+Dgrbsq1[(indx+v2+1)>>1].p)))+rbvarm);

            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            //bound the interpolation in regions of high saturation
            if (rb[indx1].p<cfa[indx])
            {
              if (xmul2f(rb[indx1].p) < cfa[indx])
              {
                rb[indx1].p = ULIM(rb[indx1].p ,cfa[indx-p1],cfa[indx+p1]);
              }
              else
              {
                pwt = xmul2f(cfa[indx]-rb[indx1].p)/(eps+rb[indx1].p+cfa[indx]);
                rb[indx1].p=pwt*rb[indx1].p + (1.0f-pwt)*ULIM(rb[indx1].p,cfa[indx-p1],cfa[indx+p1]);
              }
            }
            if (rb[indx1].m<cfa[indx])
            {
              if (xmul2f(rb[indx1].m) < cfa[indx])
              {
                rb[indx1].m = ULIM(rb[indx1].m ,cfa[indx-m1],cfa[indx+m1]);
              }
              else
              {
                mwt = xmul2f(cfa[indx]-rb[indx1].m)/(eps+rb[indx1].m+cfa[indx]);
                rb[indx1].m=mwt*rb[indx1].m + (1.0f-mwt)*ULIM(rb[indx1].m,cfa[indx-m1],cfa[indx+m1]);
              }
            }

            if (rb[indx1].p > clip_pt) rb[indx1].p=ULIM(rb[indx1].p,cfa[indx-p1],cfa[indx+p1]);//for RT implementation
            if (rb[indx1].m > clip_pt) rb[indx1].m=ULIM(rb[indx1].m,cfa[indx-m1],cfa[indx+m1]);
            //c=2-FC(rr,cc,filters);//for dcraw implementation
            //if (rbp[indx] > pre_mul[c]) rbp[indx]=ULIM(rbp[indx],cfa[indx-p1],cfa[indx+p1]);
            //if (rbm[indx] > pre_mul[c]) rbm[indx]=ULIM(rbm[indx],cfa[indx-m1],cfa[indx+m1]);
            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            //rbint[indx] = 0.5*(cfa[indx] + (rbp*rbvarm+rbm*rbvarp)/(rbvarp+rbvarm));//this is R+B, interpolated
          }



        for (rr=10; rr<rr1-10; rr++)
          for (cc=10+(FC(rr,2,filters)&1),indx=rr*TS+cc,indx1=indx>>1; cc<cc1-10; cc+=2,indx+=2,indx1++)
          {

            //first ask if one gets more directional discrimination from nearby B/R sites
            pmwtalt = xdivf(pmwt[(indx-m1)>>1]+pmwt[(indx+p1)>>1]+pmwt[(indx-p1)>>1]+pmwt[(indx+m1)>>1],2);
//					vo=fabsf(0.5-pmwt[indx1]);
//					ve=fabsf(0.5-pmwtalt);
            if (fabsf(0.5f-pmwt[indx1])<fabsf(0.5f-pmwtalt))
            {
              pmwt[indx1]=pmwtalt; //a better result was obtained from the neighbors
            }

//					if (vo<ve) {pmwt[indx1]=pmwtalt;}//a better result was obtained from the neighbors
            rbint[indx1] = xdiv2f(cfa[indx] + rb[indx1].m*(1.0f-pmwt[indx1]) + rb[indx1].p*pmwt[indx1]);//this is R+B, interpolated
          }

        for (rr=12; rr<rr1-12; rr++)
          for (cc=12+(FC(rr,2,filters)&1),indx=rr*TS+cc,indx1=indx>>1; cc<cc1-12; cc+=2,indx+=2,indx1++)
          {

            if (fabsf(0.5f-pmwt[indx>>1])<fabsf(0.5f-hvwt[indx>>1]) ) continue;

            //now interpolate G vertically/horizontally using R+B values
            //unfortunately, since G interpolation cannot be done diagonally this may lead to color shifts
            //color ratios for G interpolation

            cru = cfa[indx-v1]*2.0/(eps+rbint[indx1]+rbint[(indx1-v1)]);
            crd = cfa[indx+v1]*2.0/(eps+rbint[indx1]+rbint[(indx1+v1)]);
            crl = cfa[indx-1]*2.0/(eps+rbint[indx1]+rbint[(indx1-1)]);
            crr = cfa[indx+1]*2.0/(eps+rbint[indx1]+rbint[(indx1+1)]);

            //interpolated G via adaptive ratios or Hamilton-Adams in each cardinal direction
            if (fabsf(1.0f-cru)<arthresh)
            {
              gu=rbint[indx1]*cru;
            }
            else
            {
              gu=cfa[indx-v1]+xdiv2f(rbint[indx1]-rbint[(indx1-v1)]);
            }
            if (fabsf(1.0f-crd)<arthresh)
            {
              gd=rbint[indx1]*crd;
            }
            else
            {
              gd=cfa[indx+v1]+xdiv2f(rbint[indx1]-rbint[(indx1+v1)]);
            }
            if (fabsf(1.0f-crl)<arthresh)
            {
              gl=rbint[indx1]*crl;
            }
            else
            {
              gl=cfa[indx-1]+xdiv2f(rbint[indx1]-rbint[(indx1-1)]);
            }
            if (fabsf(1.0f-crr)<arthresh)
            {
              gr=rbint[indx1]*crr;
            }
            else
            {
              gr=cfa[indx+1]+xdiv2f(rbint[indx1]-rbint[(indx1+1)]);
            }

            //gu=rbint[indx]*cru;
            //gd=rbint[indx]*crd;
            //gl=rbint[indx]*crl;
            //gr=rbint[indx]*crr;

            //interpolated G via adaptive weights of cardinal evaluations
            Gintv = (dirwts[indx-v1][0]*gd+dirwts[indx+v1][0]*gu)/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);
            Ginth = (dirwts[indx-1][1]*gr+dirwts[indx+1][1]*gl)/(dirwts[indx-1][1]+dirwts[indx+1][1]);

            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            //bound the interpolation in regions of high saturation
            if (Gintv<rbint[indx1])
            {
              if (2*Gintv < rbint[indx1])
              {
                Gintv = ULIM(Gintv ,cfa[indx-v1],cfa[indx+v1]);
              }
              else
              {
                vwt = 2.0*(rbint[indx1]-Gintv)/(eps+Gintv+rbint[indx1]);
                Gintv=vwt*Gintv + (1.0f-vwt)*ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
              }
            }
            if (Ginth<rbint[indx1])
            {
              if (2*Ginth < rbint[indx1])
              {
                Ginth = ULIM(Ginth ,cfa[indx-1],cfa[indx+1]);
              }
              else
              {
                hwt = 2.0*(rbint[indx1]-Ginth)/(eps+Ginth+rbint[indx1]);
                Ginth=hwt*Ginth + (1.0f-hwt)*ULIM(Ginth,cfa[indx-1],cfa[indx+1]);
              }
            }

            if (Ginth > clip_pt) Ginth=ULIM(Ginth,cfa[indx-1],cfa[indx+1]);//for RT implementation
            if (Gintv > clip_pt) Gintv=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
            //c=FC(rr,cc,filters);//for dcraw implementation
            //if (Ginth > pre_mul[c]) Ginth=ULIM(Ginth,cfa[indx-1],cfa[indx+1]);
            //if (Gintv > pre_mul[c]) Gintv=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
            // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            rgb[indx][1] = Ginth*(1.0f-hvwt[indx1]) + Gintv*hvwt[indx1];
            //rgb[indx][1] = 0.5*(rgb[indx][1]+0.25*(rgb[indx-v1][1]+rgb[indx+v1][1]+rgb[indx-1][1]+rgb[indx+1][1]));
            Dgrb[indx][0] = rgb[indx][1]-cfa[indx];

            //rgb[indx][2-FC(rr,cc,filters)]=2*rbint[indx]-cfa[indx];
          }
        //end of diagonal interpolation correction
        //t2_diag += clock() - t1_diag;
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        //t1_chroma = clock();
        //fancy chrominance interpolation
        //(ey,ex) is location of R site
        for (rr=13-ey; rr<rr1-12; rr+=2)
          for (cc=13-ex,indx=rr*TS+cc; cc<cc1-12; cc+=2,indx+=2)  //B coset
          {
            Dgrb[indx][1]=Dgrb[indx][0];//split out G-B from G-R
            Dgrb[indx][0]=0;
          }
        for (rr=12; rr<rr1-12; rr++)
          for (cc=12+(FC(rr,2,filters)&1),indx=rr*TS+cc,c=1-FC(rr,cc,filters)/2; cc<cc1-12; cc+=2,indx+=2)
          {
            wtnw=1.0/(eps+fabsf(Dgrb[indx-m1][c]-Dgrb[indx+m1][c])+fabsf(Dgrb[indx-m1][c]-Dgrb[indx-m3][c])+fabsf(Dgrb[indx+m1][c]-Dgrb[indx-m3][c]));
            wtne=1.0/(eps+fabsf(Dgrb[indx+p1][c]-Dgrb[indx-p1][c])+fabsf(Dgrb[indx+p1][c]-Dgrb[indx+p3][c])+fabsf(Dgrb[indx-p1][c]-Dgrb[indx+p3][c]));
            wtsw=1.0/(eps+fabsf(Dgrb[indx-p1][c]-Dgrb[indx+p1][c])+fabsf(Dgrb[indx-p1][c]-Dgrb[indx+m3][c])+fabsf(Dgrb[indx+p1][c]-Dgrb[indx-p3][c]));
            wtse=1.0/(eps+fabsf(Dgrb[indx+m1][c]-Dgrb[indx-m1][c])+fabsf(Dgrb[indx+m1][c]-Dgrb[indx-p3][c])+fabsf(Dgrb[indx-m1][c]-Dgrb[indx+m3][c]));

            //Dgrb[indx][c]=(wtnw*Dgrb[indx-m1][c]+wtne*Dgrb[indx+p1][c]+wtsw*Dgrb[indx-p1][c]+wtse*Dgrb[indx+m1][c])/(wtnw+wtne+wtsw+wtse);

            Dgrb[indx][c]=(wtnw*(1.325*Dgrb[indx-m1][c]-0.175*Dgrb[indx-m3][c]-0.075*Dgrb[indx-m1-2][c]-0.075*Dgrb[indx-m1-v2][c] )+
                           wtne*(1.325*Dgrb[indx+p1][c]-0.175*Dgrb[indx+p3][c]-0.075*Dgrb[indx+p1+2][c]-0.075*Dgrb[indx+p1+v2][c] )+
                           wtsw*(1.325*Dgrb[indx-p1][c]-0.175*Dgrb[indx-p3][c]-0.075*Dgrb[indx-p1-2][c]-0.075*Dgrb[indx-p1-v2][c] )+
                           wtse*(1.325*Dgrb[indx+m1][c]-0.175*Dgrb[indx+m3][c]-0.075*Dgrb[indx+m1+2][c]-0.075*Dgrb[indx+m1+v2][c] ))/(wtnw+wtne+wtsw+wtse);
          }
        for (rr=12; rr<rr1-12; rr++)
          for (cc=12+(FC(rr,1,filters)&1),indx=rr*TS+cc,c=FC(rr,cc+1,filters)/2; cc<cc1-12; cc+=2,indx+=2)
            for(c=0; c<2; c++)
            {
//						Dgrb[indx][c]=((hvwt[indx-v1])*Dgrb[indx-v1][c]+(1.0f-hvwt[indx+1])*Dgrb[indx+1][c]+(1.0f-hvwt[indx-1])*Dgrb[indx-1][c]+(hvwt[indx+v1])*Dgrb[indx+v1][c])/
//						((hvwt[indx-v1])+(1.0f-hvwt[indx+1])+(1.0f-hvwt[indx-1])+(hvwt[indx+v1]));

              Dgrb[indx][c]=((hvwt[(indx-v1)>>1])*Dgrb[indx-v1][c]+(1.0f-hvwt[(indx+1)>>1])*Dgrb[indx+1][c]+(1.0f-hvwt[(indx-1)>>1])*Dgrb[indx-1][c]+(hvwt[(indx+v1)>>1])*Dgrb[indx+v1][c])/
                            ((hvwt[(indx-v1)>>1])+(1.0f-hvwt[(indx+1)>>1])+(1.0f-hvwt[(indx-1)>>1])+(hvwt[(indx+v1)>>1]));

            }
        for(rr=12; rr<rr1-12; rr++)
          for(cc=12,indx=rr*TS+cc; cc<cc1-12; cc++,indx++)
          {
            rgb[indx][0]=(rgb[indx][1]-Dgrb[indx][0]);
            rgb[indx][2]=(rgb[indx][1]-Dgrb[indx][1]);
          }
        //t2_chroma += clock() - t1_chroma;

        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

        // copy smoothed results back to image matrix
        for (rr=16; rr < rr1-16; rr++)
          for (row=rr+top, cc=16; cc < cc1-16; cc++)
          {
            col = cc + left;

            indx=rr*TS+cc;

            //red[row][col] = ((65535.0f*rgb[indx][0] ));
            //green[row][col] = ((65535.0f*rgb[indx][1]));
            //blue[row][col] = ((65535.0f*rgb[indx][2]));
            if(col < roi_out->width && row < roi_out->height)
            {
              out[(row*roi_out->width+col)*4]   = clampnan(rgb[indx][0], 0.0f, 1.0f);
              out[(row*roi_out->width+col)*4+1] = clampnan(rgb[indx][1], 0.0f, 1.0f);
              out[(row*roi_out->width+col)*4+2] = clampnan(rgb[indx][2], 0.0f, 1.0f);
            }

            //for dcraw implementation
            //for (c=0; c<3; c++){
            //	image[indx][c] = CLIP((int)(65535.0f*rgb[rr*TS+cc][c] + 0.5f));
            //}
          }
        //end of main loop

        // clean up
        //free(buffer);
        //progress+=(double)((TS-32)*(TS-32))/(height*width);
        //if (progress>1.0)
        //{
        //	progress=1.0;
        //}
        //if(plistener) plistener->setProgress(progress);
      }

    // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%



    // clean up
    free(buffer);
  }
  // done

#undef TS

}
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
