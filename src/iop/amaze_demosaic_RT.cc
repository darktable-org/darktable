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

/*==================================================================================
 * begin raw therapee code, hg checkout of april 22, 2011 branch defloat.
 *==================================================================================*/

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
	int winw =  roi_out->width;
	int winh = roi_out->height;
	int width=winw, height=winh;
	
	//const uint32_t filters = dt_image_flipped_filter(self->dev->image);
	
	//const float clip_pt = 1/initialGain;
	const float clip_pt = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

#define TS 512	 // Tile size; the image is processed in square tiles to lower memory requirements and facilitate multi-threading
	
	// local variables


	//offset of R pixel within a Bayer quartet
	int ex, ey;

	//shifts of pointer value to access pixels in vertical and diagonal directions
	static const int v1=TS, v2=2*TS, v3=3*TS, p1=-TS+1, p2=-2*TS+2, p3=-3*TS+3, m1=TS+1, m2=2*TS+2, m3=3*TS+3;

	//neighborhood of a pixel
	//static const int nbr[5] = {-v2,-2,2,v2,0};

	//tolerance to avoid dividing by zero
	static const float eps=1e-5, epssq=1e-10;			//tolerance to avoid dividing by zero

	//adaptive ratios threshold
	static const float arthresh=0.75;
	//nyquist texture test threshold
	static const float nyqthresh=0.5;
	//diagonal interpolation test threshold
	//static const float pmthresh=0.25;
	//factors for bounding interpolation in saturated regions
	//static const float lbd=1.0, ubd=1.0; //lbd=0.66, ubd=1.5 alternative values;

	//gaussian on 5x5 quincunx, sigma=1.2
	static const float gaussodd[4] = {0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f, 0.0365543548389495f};
	//gaussian on 5x5, sigma=1.2
	static const float gaussgrad[6] = {0.07384411893421103f, 0.06207511968171489f, 0.0521818194747806f, \
	0.03687419286733595f, 0.03099732204057846f, 0.018413194161458882f};
	//gaussian on 3x3, sigma =0.7
	//static const float gauss1[3] = {0.3376688223162362f, 0.12171198028231786f, 0.04387081413862306f};
	//gaussian on 5x5 alt quincunx, sigma=1.5
	static const float gausseven[2] = {0.13719494435797422f, 0.05640252782101291f};
	//guassian on quincunx grid
	static const float gquinc[4] = {0.169917f, 0.108947f, 0.069855f, 0.0287182f};

	//volatile double progress = 0.0;
	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
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
	// square of vcd
	float (*vcdsq);
	// square of hcd
	float (*hcdsq);
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
	// horizontal curvature of interpolated G (used to refine interpolation in Nyquist texture regions)
	float (*Dgrbh2);
	// vertical curvature of interpolated G
	float (*Dgrbv2);
	// difference between up/down interpolations of G
	float (*dgintv);
	// difference between left/right interpolations of G
	float (*dginth);
	// diagonal (plus) color difference R-B or G1-G2
	//float (*Dgrbp1);
	// diagonal (minus) color difference R-B or G1-G2
	//float (*Dgrbm1);
	// square of diagonal color difference
	float (*Dgrbpsq1);
	// square of diagonal color difference
	float (*Dgrbmsq1);
	// tile raw data
	float (*cfa);
	// relative weight for combining plus and minus diagonal interpolations
	float (*pmwt);
	// interpolated color difference R-B in plus direction
	float (*rbp);
	// interpolated color difference R-B in minus direction
	float (*rbm);

	// nyquist texture flag 1=nyquist, 0=not nyquist
	int   (*nyquist);


	// assign working space
	buffer = (char *) malloc((32 * sizeof(float) + sizeof(int)) * TS * TS);
	char *cur = buffer;
	//merror(buffer,"amaze_interpolate()");
	//memset(buffer,0,(34*sizeof(float)+sizeof(int))*TS*TS);
	// rgb array
	rgb			= (float (*)[3])		cur; cur += 3 * sizeof(float) * TS * TS; //pointers to array
	delh		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	delv		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	delhsq		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	delvsq		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	dirwts		= (float (*)[2])		cur; cur += 2 * sizeof(float) * TS * TS;
	vcd			= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	hcd			= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	vcdalt		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	hcdalt		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	vcdsq		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	hcdsq		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	cddiffsq	= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	hvwt		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	Dgrb		= (float (*)[2])		cur; cur += 2 * sizeof(float) * TS * TS;
	delp		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	delm		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	rbint		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	Dgrbh2		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	Dgrbv2		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	dgintv		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	dginth		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	//Dgrbp1	= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	//Dgrbm1	= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	Dgrbpsq1	= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	Dgrbmsq1	= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	cfa			= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	pmwt		= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	rbp			= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;
	rbm			= (float (*))			cur; cur += 1 * sizeof(float) * TS * TS;

	nyquist		= (int (*))				cur; cur += 1 * sizeof(int) * TS * TS;

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

	//if (plistener) {
	//	plistener->setProgressStr ("AMaZE Demosaicing...");
	//	plistener->setProgress (0.0);
	//}

	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


	//determine GRBG coset; (ey,ex) is the offset of the R subarray
	if (FC(0,0,filters)==1) {//first pixel is G
		if (FC(0,1,filters)==0) {ey=0; ex=1;} else {ey=1; ex=0;}
	} else {//first pixel is R or B
		if (FC(0,0,filters)==0) {ey=0; ex=0;} else {ey=1; ex=1;}
	}

	// Main algorithm: Tile loop
	//#pragma omp parallel for shared(rawData,height,width,red,green,blue) private(top,left) schedule(dynamic)
	//code is openmp ready; just have to pull local tile variable declarations inside the tile loop
#ifdef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
	for (top=winy-16; top < winy+height; top += TS-32)
		for (left=winx-16; left < winx+width; left += TS-32) {
			//location of tile bottom edge
			int bottom = MIN( top+TS,winy+height+16);
			//location of tile right edge
			int right  = MIN(left+TS, winx+width+16);
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
			//direction counter for nbrs[]
			/*int dir;*/
			//dummy indices
			int i, j;
			// +1 or -1
			int sgn;

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
			//gradients in various directions
			/*float gradp, gradm, gradv, gradh, gradpm, gradhv;*/
			//color difference variances in vertical and horizontal directions
			float vcdvar, hcdvar, vcdvar1, hcdvar1, hcdaltvar, vcdaltvar;
			//adaptive interpolation weight using variance of color differences
			float varwt;
			//adaptive interpolation weight using difference of left-right and up-down G interpolations
			float diffwt;
			//alternative adaptive weight for combining horizontal/vertical interpolations
			float hvwtalt;
			//temporary variables for combining interpolation weights at R and B sites
			float vo, ve;
			//interpolation of G in four directions
			float gu, gd, gl, gr;
			//variance of G in vertical/horizontal directions
			float gvarh, gvarv;

			//Nyquist texture test
			float nyqtest;
			//accumulators for Nyquist texture interpolation
			float sumh, sumv, sumsqh, sumsqv, areawt;

			//color ratios in diagonal directions
			float crse, crnw, crne, crsw;
			//color differences in diagonal directions
			float rbse, rbnw, rbne, rbsw;
			//adaptive weights for combining diagonal interpolations
			float wtse, wtnw, wtsw, wtne;
			//alternate weight for combining diagonal interpolations
			float pmwtalt;
			//variance of R-B in plus/minus directions
			float rbvarp, rbvarm;


		
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


			// rgb from input CFA data
			// rgb values should be floating point number between 0 and 1 
			// after white balance multipliers are applied 
			// a 16 pixel border is added to each side of the image

			// bookkeeping for borders
			if (top<winy) {rrmin=16;} else {rrmin=0;}
			if (left<winx) {ccmin=16;} else {ccmin=0;}
			if (bottom>(winy+height)) {rrmax=winy+height-top;} else {rrmax=rr1;}
			if (right>(winx+width)) {ccmax=winx+width-left;} else {ccmax=cc1;}
			
			for (rr=rrmin; rr < rrmax; rr++)
				for (row=rr+top, cc=ccmin; cc < ccmax; cc++) {
					col = cc+left;
					c = FC(rr,cc,filters);
					indx1=rr*TS+cc;
					//rgb[indx1][c] = (rawData[row][col])/65535.0f;
					rgb[indx1][c] = in[row*width + col];
					//indx=row*width+col;
					//rgb[indx1][c] = image[indx][c]/65535.0f;//for dcraw implementation

					cfa[indx1] = rgb[indx1][c];
				}
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			//fill borders
			if (rrmin>0) {
				for (rr=0; rr<16; rr++) 
					for (cc=ccmin; cc<ccmax; cc++) {
						c = FC(rr,cc,filters);
						rgb[rr*TS+cc][c] = rgb[(32-rr)*TS+cc][c];
						cfa[rr*TS+cc] = rgb[rr*TS+cc][c];
					}
			}
			if (rrmax<rr1) {
				for (rr=0; rr<16; rr++) 
					for (cc=ccmin; cc<ccmax; cc++) {
						c=FC(rr,cc,filters);
						//rgb[(rrmax+rr)*TS+cc][c] = (rawData[(winy+height-rr-2)][left+cc])/65535.0f;
						rgb[(rrmax+rr)*TS+cc][c] = in[(winy+height-rr-2)*width+left+cc];
						//rgb[(rrmax+rr)*TS+cc][c] = (image[(height-rr-2)*width+left+cc][c])/65535.0f;//for dcraw implementation
						cfa[(rrmax+rr)*TS+cc] = rgb[(rrmax+rr)*TS+cc][c];
					}
			}
			if (ccmin>0) {
				for (rr=rrmin; rr<rrmax; rr++) 
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						rgb[rr*TS+cc][c] = rgb[rr*TS+32-cc][c];           
						cfa[rr*TS+cc] = rgb[rr*TS+cc][c];
					}
			}
			if (ccmax<cc1) {
				for (rr=rrmin; rr<rrmax; rr++)
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						//rgb[rr*TS+ccmax+cc][c] = (rawData[(top+rr)][(winx+width-cc-2)])/65535.0f;
						rgb[rr*TS+ccmax+cc][c] = in[(top+rr)*width + (winx+width-cc-2)];
						//rgb[rr*TS+ccmax+cc][c] = (image[(top+rr)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
						cfa[rr*TS+ccmax+cc] = rgb[rr*TS+ccmax+cc][c];
					}
			}
			
			//also, fill the image corners
			if (rrmin>0 && ccmin>0) {
				for (rr=0; rr<16; rr++) 
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						//rgb[(rr)*TS+cc][c] = (rawData[winy+32-rr][winx+32-cc])/65535.0f;
						rgb[(rr)*TS+cc][c] = in[(winy+32-rr)*width + winx+32-cc];
						//rgb[(rr)*TS+cc][c] = (rgb[(32-rr)*TS+(32-cc)][c]);//for dcraw implementation
						cfa[(rr)*TS+cc] = rgb[(rr)*TS+cc][c];
					}
			}
			if (rrmax<rr1 && ccmax<cc1) {
				for (rr=0; rr<16; rr++) 
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						//rgb[(rrmax+rr)*TS+ccmax+cc][c] = (rawData[(winy+height-rr-2)][(winx+width-cc-2)])/65535.0f;
						rgb[(rrmax+rr)*TS+ccmax+cc][c] = in[(winy+height-rr-2)*width + (winx+width-cc-2)];
						//rgb[(rrmax+rr)*TS+ccmax+cc][c] = (image[(height-rr-2)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
						cfa[(rrmax+rr)*TS+ccmax+cc] = rgb[(rrmax+rr)*TS+ccmax+cc][c];
					}
			}
			if (rrmin>0 && ccmax<cc1) {
				for (rr=0; rr<16; rr++) 
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						//rgb[(rr)*TS+ccmax+cc][c] = (rawData[(winy+32-rr)][(winx+width-cc-2)])/65535.0f;
						rgb[(rr)*TS+ccmax+cc][c] = in[(winy+32-rr)*width + (winx+width-cc-2)];
						//rgb[(rr)*TS+ccmax+cc][c] = (image[(32-rr)*width+(width-cc-2)][c])/65535.0f;//for dcraw implementation
						cfa[(rr)*TS+ccmax+cc] = rgb[(rr)*TS+ccmax+cc][c];
					}
			}
			if (rrmax<rr1 && ccmin>0) {
				for (rr=0; rr<16; rr++) 
					for (cc=0; cc<16; cc++) {
						c=FC(rr,cc,filters);
						//rgb[(rrmax+rr)*TS+cc][c] = (rawData[(winy+height-rr-2)][(winx+32-cc)])/65535.0f;
						rgb[(rrmax+rr)*TS+cc][c] = in[(winy+height-rr-2)*width + (winx+32-cc)];
						//rgb[(rrmax+rr)*TS+cc][c] = (image[(height-rr-2)*width+(32-cc)][c])/65535.0f;//for dcraw implementation
						cfa[(rrmax+rr)*TS+cc] = rgb[(rrmax+rr)*TS+cc][c];
					}
			}

			//end of border fill
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

			for (rr=1; rr < rr1-1; rr++)
				for (cc=1, indx=(rr)*TS+cc; cc < cc1-1; cc++, indx++) {
					
					delh[indx] = fabs(cfa[indx+1]-cfa[indx-1]);
					delv[indx] = fabs(cfa[indx+v1]-cfa[indx-v1]);
					delhsq[indx] = SQR(delh[indx]);
					delvsq[indx] = SQR(delv[indx]);
					delp[indx] = fabs(cfa[indx+p1]-cfa[indx-p1]);
					delm[indx] = fabs(cfa[indx+m1]-cfa[indx-m1]);
					
				}

			for (rr=2; rr < rr1-2; rr++)
				for (cc=2,indx=(rr)*TS+cc; cc < cc1-2; cc++, indx++) {
					
					dirwts[indx][0] = eps+delv[indx+v1]+delv[indx-v1]+delv[indx];//+fabs(cfa[indx+v2]-cfa[indx-v2]);
					//vert directional averaging weights
					dirwts[indx][1] = eps+delh[indx+1]+delh[indx-1]+delh[indx];//+fabs(cfa[indx+2]-cfa[indx-2]);
					//horizontal weights
					
					if (FC(rr,cc,filters)&1) {
						//for later use in diagonal interpolation
						//Dgrbp1[indx]=2*cfa[indx]-(cfa[indx-p1]+cfa[indx+p1]);
						//Dgrbm1[indx]=2*cfa[indx]-(cfa[indx-m1]+cfa[indx+m1]);
						Dgrbpsq1[indx]=(SQR(cfa[indx]-cfa[indx-p1])+SQR(cfa[indx]-cfa[indx+p1]));
						Dgrbmsq1[indx]=(SQR(cfa[indx]-cfa[indx-m1])+SQR(cfa[indx]-cfa[indx+m1]));
					} 
				}

			//t2_init += clock()-t1_init;
			// end of tile initialization
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

			//interpolate vertical and horizontal color differences
			//t1_vcdhcd = clock();

			for (rr=4; rr<rr1-4; rr++)
				//for (cc=4+(FC(rr,2)&1),indx=rr*TS+cc,c=FC(rr,cc); cc<cc1-4; cc+=2,indx+=2) {
				for (cc=4,indx=rr*TS+cc; cc<cc1-4; cc++,indx++) {
					c=FC(rr,cc,filters);
					if (c&1) {sgn=-1;} else {sgn=1;}

					//initialization of nyquist test
					nyquist[indx]=0;
					//preparation for diag interp
					rbint[indx]=0;

					//color ratios in each cardinal direction
					cru = cfa[indx-v1]*(dirwts[indx-v2][0]+dirwts[indx][0])/(dirwts[indx-v2][0]*(eps+cfa[indx])+dirwts[indx][0]*(eps+cfa[indx-v2]));
					crd = cfa[indx+v1]*(dirwts[indx+v2][0]+dirwts[indx][0])/(dirwts[indx+v2][0]*(eps+cfa[indx])+dirwts[indx][0]*(eps+cfa[indx+v2]));
					crl = cfa[indx-1]*(dirwts[indx-2][1]+dirwts[indx][1])/(dirwts[indx-2][1]*(eps+cfa[indx])+dirwts[indx][1]*(eps+cfa[indx-2]));
					crr = cfa[indx+1]*(dirwts[indx+2][1]+dirwts[indx][1])/(dirwts[indx+2][1]*(eps+cfa[indx])+dirwts[indx][1]*(eps+cfa[indx+2]));

					guha=HCLIP(cfa[indx-v1])+0.5*(cfa[indx]-cfa[indx-v2]);
					gdha=HCLIP(cfa[indx+v1])+0.5*(cfa[indx]-cfa[indx+v2]);
					glha=HCLIP(cfa[indx-1])+0.5*(cfa[indx]-cfa[indx-2]);
					grha=HCLIP(cfa[indx+1])+0.5*(cfa[indx]-cfa[indx+2]);

					if (fabs(1.0f-cru)<arthresh) {guar=cfa[indx]*cru;} else {guar=guha;}
					if (fabs(1.0f-crd)<arthresh) {gdar=cfa[indx]*crd;} else {gdar=gdha;}
					if (fabs(1.0f-crl)<arthresh) {glar=cfa[indx]*crl;} else {glar=glha;}
					if (fabs(1.0f-crr)<arthresh) {grar=cfa[indx]*crr;} else {grar=grha;}
					
					hwt = dirwts[indx-1][1]/(dirwts[indx-1][1]+dirwts[indx+1][1]);
					vwt = dirwts[indx-v1][0]/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);

					//interpolated G via adaptive weights of cardinal evaluations
					Gintvar = vwt*gdar+(1.0f-vwt)*guar;
					Ginthar = hwt*grar+(1.0f-hwt)*glar;
					Gintvha = vwt*gdha+(1.0f-vwt)*guha;
					Ginthha = hwt*grha+(1.0f-hwt)*glha;
					//interpolated color differences
					vcd[indx] = sgn*(Gintvar-cfa[indx]);
					hcd[indx] = sgn*(Ginthar-cfa[indx]);
					vcdalt[indx] = sgn*(Gintvha-cfa[indx]);
					hcdalt[indx] = sgn*(Ginthha-cfa[indx]);
					
					if (cfa[indx] > 0.8*clip_pt || Gintvha > 0.8*clip_pt || Ginthha > 0.8*clip_pt) {
						//use HA if highlights are (nearly) clipped
						guar=guha; gdar=gdha; glar=glha; grar=grha;
						vcd[indx]=vcdalt[indx]; hcd[indx]=hcdalt[indx];
					}

					//differences of interpolations in opposite directions
					dgintv[indx]=MIN(SQR(guha-gdha),SQR(guar-gdar));
					dginth[indx]=MIN(SQR(glha-grha),SQR(glar-grar));
					
					//dgintv[indx]=SQR(guar-gdar);
					//dginth[indx]=SQR(glar-grar);
					
					//vcdsq[indx] = SQR(vcd[indx]);
					//hcdsq[indx] = SQR(hcd[indx]);
					//cddiffsq[indx] = SQR(vcd[indx]-hcd[indx]);
				}
			//t2_vcdhcd += clock() - t1_vcdhcd;

			//t1_cdvar = clock();
			for (rr=4; rr<rr1-4; rr++)
				//for (cc=4+(FC(rr,2)&1),indx=rr*TS+cc,c=FC(rr,cc); cc<cc1-4; cc+=2,indx+=2) {
				for (cc=4,indx=rr*TS+cc; cc<cc1-4; cc++,indx++) {
					c=FC(rr,cc,filters);

					hcdvar =3.0f*(SQR(hcd[indx-2])+SQR(hcd[indx])+SQR(hcd[indx+2]))-SQR(hcd[indx-2]+hcd[indx]+hcd[indx+2]);
					hcdaltvar =3.0f*(SQR(hcdalt[indx-2])+SQR(hcdalt[indx])+SQR(hcdalt[indx+2]))-SQR(hcdalt[indx-2]+hcdalt[indx]+hcdalt[indx+2]);
					vcdvar =3.0f*(SQR(vcd[indx-v2])+SQR(vcd[indx])+SQR(vcd[indx+v2]))-SQR(vcd[indx-v2]+vcd[indx]+vcd[indx+v2]);
					vcdaltvar =3.0f*(SQR(vcdalt[indx-v2])+SQR(vcdalt[indx])+SQR(vcdalt[indx+v2]))-SQR(vcdalt[indx-v2]+vcdalt[indx]+vcdalt[indx+v2]);
					//choose the smallest variance; this yields a smoother interpolation
					if (hcdaltvar<hcdvar) hcd[indx]=hcdalt[indx];
					if (vcdaltvar<vcdvar) vcd[indx]=vcdalt[indx];
					
					//bound the interpolation in regions of high saturation
					if (c&1) {//G site
						Ginth = -hcd[indx]+cfa[indx];//R or B
						Gintv = -vcd[indx]+cfa[indx];//B or R

						if (hcd[indx]>0) {
							if (3.0f*hcd[indx] > (Ginth+cfa[indx])) {
								hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];
							} else {
								hwt = 1.0f -3.0f*hcd[indx]/(eps+Ginth+cfa[indx]);
								hcd[indx]=hwt*hcd[indx] + (1.0f-hwt)*(-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx]);
							}
						}
						if (vcd[indx]>0) {
							if (3.0f*vcd[indx] > (Gintv+cfa[indx])) {
								vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];
							} else {
								vwt = 1.0f -3.0f*vcd[indx]/(eps+Gintv+cfa[indx]);
								vcd[indx]=vwt*vcd[indx] + (1.0f-vwt)*(-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx]);
							}
						}
						
						if (Ginth > clip_pt) hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];//for RT implementation
						if (Gintv > clip_pt) vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];
						//if (Ginth > pre_mul[c]) hcd[indx]=-ULIM(Ginth,cfa[indx-1],cfa[indx+1])+cfa[indx];//for dcraw implementation
						//if (Gintv > pre_mul[c]) vcd[indx]=-ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])+cfa[indx];

					} else {//R or B site

						Ginth = hcd[indx]+cfa[indx];//interpolated G
						Gintv = vcd[indx]+cfa[indx];

						if (hcd[indx]<0) {
							if (3.0f*hcd[indx] < -(Ginth+cfa[indx])) {
								hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];
							} else {
								hwt = 1.0f +3.0f*hcd[indx]/(eps+Ginth+cfa[indx]);
								hcd[indx]=hwt*hcd[indx] + (1.0f-hwt)*(ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx]);
							}
						}
						if (vcd[indx]<0) {
							if (3.0f*vcd[indx] < -(Gintv+cfa[indx])) {
								vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
							} else {
								vwt = 1.0f +3.0f*vcd[indx]/(eps+Gintv+cfa[indx]);
								vcd[indx]=vwt*vcd[indx] + (1.0f-vwt)*(ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx]);
							}
						}

						if (Ginth > clip_pt) hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];//for RT implementation
						if (Gintv > clip_pt) vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
						//if (Ginth > pre_mul[c]) hcd[indx]=ULIM(Ginth,cfa[indx-1],cfa[indx+1])-cfa[indx];//for dcraw implementation
						//if (Gintv > pre_mul[c]) vcd[indx]=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1])-cfa[indx];
					}
					
					
					vcdsq[indx] = SQR(vcd[indx]);
					hcdsq[indx] = SQR(hcd[indx]);
					cddiffsq[indx] = SQR(vcd[indx]-hcd[indx]);
				}

			for (rr=6; rr<rr1-6; rr++)
				for (cc=6+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-6; cc+=2,indx+=2) {

					//compute color difference variances in cardinal directions

					
					Dgrbvvaru = 4*(vcdsq[indx]+vcdsq[indx-v1]+vcdsq[indx-v2]+vcdsq[indx-v3])-SQR(vcd[indx]+vcd[indx-v1]+vcd[indx-v2]+vcd[indx-v3]);
					Dgrbvvard = 4*(vcdsq[indx]+vcdsq[indx+v1]+vcdsq[indx+v2]+vcdsq[indx+v3])-SQR(vcd[indx]+vcd[indx+v1]+vcd[indx+v2]+vcd[indx+v3]);
					Dgrbhvarl = 4*(hcdsq[indx]+hcdsq[indx-1]+hcdsq[indx-2]+hcdsq[indx-3])-SQR(hcd[indx]+hcd[indx-1]+hcd[indx-2]+hcd[indx-3]);
					Dgrbhvarr = 4*(hcdsq[indx]+hcdsq[indx+1]+hcdsq[indx+2]+hcdsq[indx+3])-SQR(hcd[indx]+hcd[indx+1]+hcd[indx+2]+hcd[indx+3]);
					
					
					hwt = dirwts[indx-1][1]/(dirwts[indx-1][1]+dirwts[indx+1][1]);
					vwt = dirwts[indx-v1][0]/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);
					
					vcdvar = epssq+vwt*Dgrbvvard+(1.0f-vwt)*Dgrbvvaru;
					hcdvar = epssq+hwt*Dgrbhvarr+(1.0f-hwt)*Dgrbhvarl;
					
					//vcdvar = 5*(vcdsq[indx]+vcdsq[indx-v1]+vcdsq[indx-v2]+vcdsq[indx+v1]+vcdsq[indx+v2])-SQR(vcd[indx]+vcd[indx-v1]+vcd[indx-v2]+vcd[indx+v1]+vcd[indx+v2]);
					//hcdvar = 5*(hcdsq[indx]+hcdsq[indx-1]+hcdsq[indx-2]+hcdsq[indx+1]+hcdsq[indx+2])-SQR(hcd[indx]+hcd[indx-1]+hcd[indx-2]+hcd[indx+1]+hcd[indx+2]);


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
					if ((0.5-varwt)*(0.5-diffwt)>0 && fabs(0.5-diffwt)<fabs(0.5-varwt)) {hvwt[indx]=varwt;} else {hvwt[indx]=diffwt;}
					
					//hvwt[indx]=varwt;
				}
			//t2_cdvar += clock() - t1_cdvar;

			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// Nyquist test				 
			//t1_nyqtest = clock();

			for (rr=6; rr<rr1-6; rr++)
				for (cc=6+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-6; cc+=2,indx+=2) {

					//nyquist texture test: ask if difference of vcd compared to hcd is larger or smaller than RGGB gradients
					nyqtest = (gaussodd[0]*cddiffsq[indx]+ \
							   gaussodd[1]*(cddiffsq[indx-m1]+cddiffsq[indx+p1]+ \
											cddiffsq[indx-p1]+cddiffsq[indx+m1])+ \
							   gaussodd[2]*(cddiffsq[indx-v2]+cddiffsq[indx-2]+ \
											cddiffsq[indx+2]+cddiffsq[indx+v2])+ \
							   gaussodd[3]*(cddiffsq[indx-m2]+cddiffsq[indx+p2]+ \
											cddiffsq[indx-p2]+cddiffsq[indx+m2]));

					nyqtest -= nyqthresh*(gaussgrad[0]*(delhsq[indx]+delvsq[indx])+ \
										  gaussgrad[1]*(delhsq[indx-v1]+delvsq[indx-v1]+delhsq[indx+1]+delvsq[indx+1]+ \
														delhsq[indx-1]+delvsq[indx-1]+delhsq[indx+v1]+delvsq[indx+v1])+ \
										  gaussgrad[2]*(delhsq[indx-m1]+delvsq[indx-m1]+delhsq[indx+p1]+delvsq[indx+p1]+ \
														delhsq[indx-p1]+delvsq[indx-p1]+delhsq[indx+m1]+delvsq[indx+m1])+ \
										  gaussgrad[3]*(delhsq[indx-v2]+delvsq[indx-v2]+delhsq[indx-2]+delvsq[indx-2]+ \
														delhsq[indx+2]+delvsq[indx+2]+delhsq[indx+v2]+delvsq[indx+v2])+ \
										  gaussgrad[4]*(delhsq[indx-2*TS-1]+delvsq[indx-2*TS-1]+delhsq[indx-2*TS+1]+delvsq[indx-2*TS+1]+ \
														delhsq[indx-TS-2]+delvsq[indx-TS-2]+delhsq[indx-TS+2]+delvsq[indx-TS+2]+ \
														delhsq[indx+TS-2]+delvsq[indx+TS-2]+delhsq[indx+TS+2]+delvsq[indx-TS+2]+ \
														delhsq[indx+2*TS-1]+delvsq[indx+2*TS-1]+delhsq[indx+2*TS+1]+delvsq[indx+2*TS+1])+ \
										  gaussgrad[5]*(delhsq[indx-m2]+delvsq[indx-m2]+delhsq[indx+p2]+delvsq[indx+p2]+ \
														delhsq[indx-p2]+delvsq[indx-p2]+delhsq[indx+m2]+delvsq[indx+m2]));


					if (nyqtest>0) {nyquist[indx]=1;}//nyquist=1 for nyquist region
				}

			for (rr=8; rr<rr1-8; rr++)
				for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2) {
					
					areawt=(nyquist[indx-v2]+nyquist[indx-m1]+nyquist[indx+p1]+ \
							nyquist[indx-2]+nyquist[indx]+nyquist[indx+2]+ \
							nyquist[indx-p1]+nyquist[indx+m1]+nyquist[indx+v2]);
					//if most of your neighbors are named Nyquist, it's likely that you're one too
					if (areawt>4) nyquist[indx]=1;
					//or not
					if (areawt<4) nyquist[indx]=0;
				}

			//t2_nyqtest += clock() - t1_nyqtest;
			// end of Nyquist test
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%




			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// in areas of Nyquist texture, do area interpolation
			//t1_areainterp = clock();
			for (rr=8; rr<rr1-8; rr++)
				for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2) {

					if (nyquist[indx]) {
						// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
						// area interpolation

						sumh=sumv=sumsqh=sumsqv=areawt=0;
						for (i=-6; i<7; i+=2)
							for (j=-6; j<7; j+=2) {
								indx1=(rr+i)*TS+cc+j;
								if (nyquist[indx1]) {
									sumh += cfa[indx1]-0.5*(cfa[indx1-1]+cfa[indx1+1]);
									sumv += cfa[indx1]-0.5*(cfa[indx1-v1]+cfa[indx1+v1]);
									sumsqh += 0.5*(SQR(cfa[indx1]-cfa[indx1-1])+SQR(cfa[indx1]-cfa[indx1+1]));
									sumsqv += 0.5*(SQR(cfa[indx1]-cfa[indx1-v1])+SQR(cfa[indx1]-cfa[indx1+v1]));
									areawt +=1;
								}
							}

						//horizontal and vertical color differences, and adaptive weight
						hcdvar=epssq+MAX(0, areawt*sumsqh-sumh*sumh);
						vcdvar=epssq+MAX(0, areawt*sumsqv-sumv*sumv);
						hvwt[indx]=hcdvar/(vcdvar+hcdvar);

						// end of area interpolation
						// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

					}
				}
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			//t2_areainterp += clock() - t1_areainterp;

			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			//populate G at R/B sites
			for (rr=8; rr<rr1-8; rr++)
				for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2) {

					//first ask if one gets more directional discrimination from nearby B/R sites
					hvwtalt = 0.25*(hvwt[indx-m1]+hvwt[indx+p1]+hvwt[indx-p1]+hvwt[indx+m1]);
					vo=fabs(0.5-hvwt[indx]);
					ve=fabs(0.5-hvwtalt);
					if (vo<ve) {hvwt[indx]=hvwtalt;}//a better result was obtained from the neighbors
					
					
					
					Dgrb[indx][0] = (hcd[indx]*(1.0f-hvwt[indx]) + vcd[indx]*hvwt[indx]);//evaluate color differences
					//if (hvwt[indx]<0.5) Dgrb[indx][0]=hcd[indx];
					//if (hvwt[indx]>0.5) Dgrb[indx][0]=vcd[indx];
					rgb[indx][1] = cfa[indx] + Dgrb[indx][0];//evaluate G (finally!)

					//local curvature in G (preparation for nyquist refinement step)
					if (nyquist[indx]) {
						Dgrbh2[indx] = SQR(rgb[indx][1] - 0.5*(rgb[indx-1][1]+rgb[indx+1][1]));
						Dgrbv2[indx] = SQR(rgb[indx][1] - 0.5*(rgb[indx-v1][1]+rgb[indx+v1][1]));
					} else {
						Dgrbh2[indx] = Dgrbv2[indx] = 0;
					}
				}

			//end of standard interpolation
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// refine Nyquist areas using G curvatures

			for (rr=8; rr<rr1-8; rr++)
				for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2) {

					if (nyquist[indx]) {
						//local averages (over Nyquist pixels only) of G curvature squared 
						gvarh = epssq + (gquinc[0]*Dgrbh2[indx]+ \
									   gquinc[1]*(Dgrbh2[indx-m1]+Dgrbh2[indx+p1]+Dgrbh2[indx-p1]+Dgrbh2[indx+m1])+ \
									   gquinc[2]*(Dgrbh2[indx-v2]+Dgrbh2[indx-2]+Dgrbh2[indx+2]+Dgrbh2[indx+v2])+ \
									   gquinc[3]*(Dgrbh2[indx-m2]+Dgrbh2[indx+p2]+Dgrbh2[indx-p2]+Dgrbh2[indx+m2]));
						gvarv = epssq + (gquinc[0]*Dgrbv2[indx]+ \
									   gquinc[1]*(Dgrbv2[indx-m1]+Dgrbv2[indx+p1]+Dgrbv2[indx-p1]+Dgrbv2[indx+m1])+ \
									   gquinc[2]*(Dgrbv2[indx-v2]+Dgrbv2[indx-2]+Dgrbv2[indx+2]+Dgrbv2[indx+v2])+ \
									   gquinc[3]*(Dgrbv2[indx-m2]+Dgrbv2[indx+p2]+Dgrbv2[indx-p2]+Dgrbv2[indx+m2]));
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
				for (cc=8+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-8; cc+=2,indx+=2) {


					rbvarp = epssq + (gausseven[0]*(Dgrbpsq1[indx-v1]+Dgrbpsq1[indx-1]+Dgrbpsq1[indx+1]+Dgrbpsq1[indx+v1]) + \
									gausseven[1]*(Dgrbpsq1[indx-v2-1]+Dgrbpsq1[indx-v2+1]+Dgrbpsq1[indx-2-v1]+Dgrbpsq1[indx+2-v1]+ \
												  Dgrbpsq1[indx-2+v1]+Dgrbpsq1[indx+2+v1]+Dgrbpsq1[indx+v2-1]+Dgrbpsq1[indx+v2+1]));
					/*rbvarp -=  SQR( (gausseven[0]*(Dgrbp1[indx-v1]+Dgrbp1[indx-1]+Dgrbp1[indx+1]+Dgrbp1[indx+v1]) + \
					gausseven[1]*(Dgrbp1[indx-v2-1]+Dgrbp1[indx-v2+1]+Dgrbp1[indx-2-v1]+Dgrbp1[indx+2-v1]+ \
					Dgrbp1[indx-2+v1]+Dgrbp1[indx+2+v1]+Dgrbp1[indx+v2-1]+Dgrbp1[indx+v2+1])));*/
					rbvarm = epssq + (gausseven[0]*(Dgrbmsq1[indx-v1]+Dgrbmsq1[indx-1]+Dgrbmsq1[indx+1]+Dgrbmsq1[indx+v1]) + \
									gausseven[1]*(Dgrbmsq1[indx-v2-1]+Dgrbmsq1[indx-v2+1]+Dgrbmsq1[indx-2-v1]+Dgrbmsq1[indx+2-v1]+ \
												  Dgrbmsq1[indx-2+v1]+Dgrbmsq1[indx+2+v1]+Dgrbmsq1[indx+v2-1]+Dgrbmsq1[indx+v2+1]));
					/*rbvarm -=  SQR( (gausseven[0]*(Dgrbm1[indx-v1]+Dgrbm1[indx-1]+Dgrbm1[indx+1]+Dgrbm1[indx+v1]) + \
					gausseven[1]*(Dgrbm1[indx-v2-1]+Dgrbm1[indx-v2+1]+Dgrbm1[indx-2-v1]+Dgrbm1[indx+2-v1]+ \
					Dgrbm1[indx-2+v1]+Dgrbm1[indx+2+v1]+Dgrbm1[indx+v2-1]+Dgrbm1[indx+v2+1])));*/



					// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

					//diagonal color ratios
					crse=2.0f*(cfa[indx+m1])/(eps+cfa[indx]+(cfa[indx+m2]));
					crnw=2.0f*(cfa[indx-m1])/(eps+cfa[indx]+(cfa[indx-m2]));
					crne=2.0f*(cfa[indx+p1])/(eps+cfa[indx]+(cfa[indx+p2]));
					crsw=2.0f*(cfa[indx-p1])/(eps+cfa[indx]+(cfa[indx-p2]));

					//assign B/R at R/B sites
					if (fabs(1.0f-crse)<arthresh) {rbse=cfa[indx]*crse;}//use this if more precise diag interp is necessary
					else {rbse=(cfa[indx+m1])+0.5*(cfa[indx]-cfa[indx+m2]);}
					if (fabs(1.0f-crnw)<arthresh) {rbnw=cfa[indx]*crnw;}
					else {rbnw=(cfa[indx-m1])+0.5*(cfa[indx]-cfa[indx-m2]);}
					if (fabs(1.0f-crne)<arthresh) {rbne=cfa[indx]*crne;}
					else {rbne=(cfa[indx+p1])+0.5*(cfa[indx]-cfa[indx+p2]);}
					if (fabs(1.0f-crsw)<arthresh) {rbsw=cfa[indx]*crsw;}
					else {rbsw=(cfa[indx-p1])+0.5*(cfa[indx]-cfa[indx-p2]);}

					wtse= eps+delm[indx]+delm[indx+m1]+delm[indx+m2];//same as for wtu,wtd,wtl,wtr
					wtnw= eps+delm[indx]+delm[indx-m1]+delm[indx-m2];
					wtne= eps+delp[indx]+delp[indx+p1]+delp[indx+p2];
					wtsw= eps+delp[indx]+delp[indx-p1]+delp[indx-p2];


					rbm[indx] = (wtse*rbnw+wtnw*rbse)/(wtse+wtnw);
					rbp[indx] = (wtne*rbsw+wtsw*rbne)/(wtne+wtsw);

					pmwt[indx] = rbvarm/(rbvarp+rbvarm);

					// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
					//bound the interpolation in regions of high saturation
					if (rbp[indx]<cfa[indx]) {
						if (2.0*rbp[indx] < cfa[indx]) {
							rbp[indx] = ULIM(rbp[indx] ,cfa[indx-p1],cfa[indx+p1]);
						} else {
							pwt = 2.0*(cfa[indx]-rbp[indx])/(eps+rbp[indx]+cfa[indx]);
							rbp[indx]=pwt*rbp[indx] + (1.0f-pwt)*ULIM(rbp[indx],cfa[indx-p1],cfa[indx+p1]);
						}
					}
					if (rbm[indx]<cfa[indx]) {
						if (2.0*rbm[indx] < cfa[indx]) {
							rbm[indx] = ULIM(rbm[indx] ,cfa[indx-m1],cfa[indx+m1]);
						} else {
							mwt = 2.0*(cfa[indx]-rbm[indx])/(eps+rbm[indx]+cfa[indx]);
							rbm[indx]=mwt*rbm[indx] + (1.0f-mwt)*ULIM(rbm[indx],cfa[indx-m1],cfa[indx+m1]);
						}
					}

					if (rbp[indx] > clip_pt) rbp[indx]=ULIM(rbp[indx],cfa[indx-p1],cfa[indx+p1]);//for RT implementation
					if (rbm[indx] > clip_pt) rbm[indx]=ULIM(rbm[indx],cfa[indx-m1],cfa[indx+m1]);
					//c=2-FC(rr,cc);//for dcraw implementation
					//if (rbp[indx] > pre_mul[c]) rbp[indx]=ULIM(rbp[indx],cfa[indx-p1],cfa[indx+p1]);
					//if (rbm[indx] > pre_mul[c]) rbm[indx]=ULIM(rbm[indx],cfa[indx-m1],cfa[indx+m1]);
					// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

					//rbint[indx] = 0.5*(cfa[indx] + (rbp*rbvarm+rbm*rbvarp)/(rbvarp+rbvarm));//this is R+B, interpolated
				}



			for (rr=10; rr<rr1-10; rr++)
				for (cc=10+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-10; cc+=2,indx+=2) {

					//first ask if one gets more directional discrimination from nearby B/R sites
					pmwtalt = 0.25*(pmwt[indx-m1]+pmwt[indx+p1]+pmwt[indx-p1]+pmwt[indx+m1]);
					vo=fabs(0.5-pmwt[indx]);
					ve=fabs(0.5-pmwtalt);
					if (vo<ve) {pmwt[indx]=pmwtalt;}//a better result was obtained from the neighbors
					rbint[indx] = 0.5*(cfa[indx] + rbm[indx]*(1.0f-pmwt[indx]) + rbp[indx]*pmwt[indx]);//this is R+B, interpolated
				}

			for (rr=12; rr<rr1-12; rr++)
				for (cc=12+(FC(rr,2,filters)&1),indx=rr*TS+cc; cc<cc1-12; cc+=2,indx+=2) {	
						
					if (fabs(0.5-pmwt[indx])<fabs(0.5-hvwt[indx]) ) continue;

					//now interpolate G vertically/horizontally using R+B values
					//unfortunately, since G interpolation cannot be done diagonally this may lead to color shifts
					//color ratios for G interpolation

					cru = cfa[indx-v1]*2.0/(eps+rbint[indx]+rbint[indx-v2]);
					crd = cfa[indx+v1]*2.0/(eps+rbint[indx]+rbint[indx+v2]);
					crl = cfa[indx-1]*2.0/(eps+rbint[indx]+rbint[indx-2]);
					crr = cfa[indx+1]*2.0/(eps+rbint[indx]+rbint[indx+2]);

					//interpolated G via adaptive ratios or Hamilton-Adams in each cardinal direction
					if (fabs(1.0f-cru)<arthresh) {gu=rbint[indx]*cru;}
					else {gu=cfa[indx-v1]+0.5*(rbint[indx]-rbint[indx-v2]);}
					if (fabs(1.0f-crd)<arthresh) {gd=rbint[indx]*crd;}
					else {gd=cfa[indx+v1]+0.5*(rbint[indx]-rbint[indx+v2]);}
					if (fabs(1.0f-crl)<arthresh) {gl=rbint[indx]*crl;}
					else {gl=cfa[indx-1]+0.5*(rbint[indx]-rbint[indx-2]);}
					if (fabs(1.0f-crr)<arthresh) {gr=rbint[indx]*crr;}
					else {gr=cfa[indx+1]+0.5*(rbint[indx]-rbint[indx+2]);}

					//gu=rbint[indx]*cru;
					//gd=rbint[indx]*crd;
					//gl=rbint[indx]*crl;
					//gr=rbint[indx]*crr;

					//interpolated G via adaptive weights of cardinal evaluations
					Gintv = (dirwts[indx-v1][0]*gd+dirwts[indx+v1][0]*gu)/(dirwts[indx+v1][0]+dirwts[indx-v1][0]);
					Ginth = (dirwts[indx-1][1]*gr+dirwts[indx+1][1]*gl)/(dirwts[indx-1][1]+dirwts[indx+1][1]);

					// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
					//bound the interpolation in regions of high saturation
					if (Gintv<rbint[indx]) {
						if (2*Gintv < rbint[indx]) {
							Gintv = ULIM(Gintv ,cfa[indx-v1],cfa[indx+v1]);
						} else {
							vwt = 2.0*(rbint[indx]-Gintv)/(eps+Gintv+rbint[indx]);
							Gintv=vwt*Gintv + (1.0f-vwt)*ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
						}
					}
					if (Ginth<rbint[indx]) {
						if (2*Ginth < rbint[indx]) {
							Ginth = ULIM(Ginth ,cfa[indx-1],cfa[indx+1]);
						} else {
							hwt = 2.0*(rbint[indx]-Ginth)/(eps+Ginth+rbint[indx]);
							Ginth=hwt*Ginth + (1.0f-hwt)*ULIM(Ginth,cfa[indx-1],cfa[indx+1]);
						}
					}

					if (Ginth > clip_pt) Ginth=ULIM(Ginth,cfa[indx-1],cfa[indx+1]);//for RT implementation
					if (Gintv > clip_pt) Gintv=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
					//c=FC(rr,cc);//for dcraw implementation
					//if (Ginth > pre_mul[c]) Ginth=ULIM(Ginth,cfa[indx-1],cfa[indx+1]);
					//if (Gintv > pre_mul[c]) Gintv=ULIM(Gintv,cfa[indx-v1],cfa[indx+v1]);
					// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

					rgb[indx][1] = Ginth*(1.0f-hvwt[indx]) + Gintv*hvwt[indx];
					//rgb[indx][1] = 0.5*(rgb[indx][1]+0.25*(rgb[indx-v1][1]+rgb[indx+v1][1]+rgb[indx-1][1]+rgb[indx+1][1]));
					Dgrb[indx][0] = rgb[indx][1]-cfa[indx];
					
					//rgb[indx][2-FC(rr,cc)]=2*rbint[indx]-cfa[indx];
				}
			//end of diagonal interpolation correction
			//t2_diag += clock() - t1_diag;
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

			//t1_chroma = clock();
			//fancy chrominance interpolation
			//(ey,ex) is location of R site
			for (rr=13-ey; rr<rr1-12; rr+=2)
				for (cc=13-ex,indx=rr*TS+cc; cc<cc1-12; cc+=2,indx+=2) {//B coset
					Dgrb[indx][1]=Dgrb[indx][0];//split out G-B from G-R
					Dgrb[indx][0]=0;
				}
			for (rr=12; rr<rr1-12; rr++)
				for (cc=12+(FC(rr,2,filters)&1),indx=rr*TS+cc,c=1-FC(rr,cc,filters)/2; cc<cc1-12; cc+=2,indx+=2) {
					wtnw=1.0/(eps+fabs(Dgrb[indx-m1][c]-Dgrb[indx+m1][c])+fabs(Dgrb[indx-m1][c]-Dgrb[indx-m3][c])+fabs(Dgrb[indx+m1][c]-Dgrb[indx-m3][c]));
					wtne=1.0/(eps+fabs(Dgrb[indx+p1][c]-Dgrb[indx-p1][c])+fabs(Dgrb[indx+p1][c]-Dgrb[indx+p3][c])+fabs(Dgrb[indx-p1][c]-Dgrb[indx+p3][c]));
					wtsw=1.0/(eps+fabs(Dgrb[indx-p1][c]-Dgrb[indx+p1][c])+fabs(Dgrb[indx-p1][c]-Dgrb[indx+m3][c])+fabs(Dgrb[indx+p1][c]-Dgrb[indx-p3][c]));
					wtse=1.0/(eps+fabs(Dgrb[indx+m1][c]-Dgrb[indx-m1][c])+fabs(Dgrb[indx+m1][c]-Dgrb[indx-p3][c])+fabs(Dgrb[indx-m1][c]-Dgrb[indx+m3][c]));

					//Dgrb[indx][c]=(wtnw*Dgrb[indx-m1][c]+wtne*Dgrb[indx+p1][c]+wtsw*Dgrb[indx-p1][c]+wtse*Dgrb[indx+m1][c])/(wtnw+wtne+wtsw+wtse);

					Dgrb[indx][c]=(wtnw*(1.325*Dgrb[indx-m1][c]-0.175*Dgrb[indx-m3][c]-0.075*Dgrb[indx-m1-2][c]-0.075*Dgrb[indx-m1-v2][c] )+ \
								   wtne*(1.325*Dgrb[indx+p1][c]-0.175*Dgrb[indx+p3][c]-0.075*Dgrb[indx+p1+2][c]-0.075*Dgrb[indx+p1+v2][c] )+ \
								   wtsw*(1.325*Dgrb[indx-p1][c]-0.175*Dgrb[indx-p3][c]-0.075*Dgrb[indx-p1-2][c]-0.075*Dgrb[indx-p1-v2][c] )+ \
								   wtse*(1.325*Dgrb[indx+m1][c]-0.175*Dgrb[indx+m3][c]-0.075*Dgrb[indx+m1+2][c]-0.075*Dgrb[indx+m1+v2][c] ))/(wtnw+wtne+wtsw+wtse);
				}
			for (rr=12; rr<rr1-12; rr++)
				for (cc=12+(FC(rr,1,filters)&1),indx=rr*TS+cc,c=FC(rr,cc+1,filters)/2; cc<cc1-12; cc+=2,indx+=2)
					for(c=0;c<2;c++){

						Dgrb[indx][c]=((hvwt[indx-v1])*Dgrb[indx-v1][c]+(1.0f-hvwt[indx+1])*Dgrb[indx+1][c]+(1.0f-hvwt[indx-1])*Dgrb[indx-1][c]+(hvwt[indx+v1])*Dgrb[indx+v1][c])/ \
						((hvwt[indx-v1])+(1.0f-hvwt[indx+1])+(1.0f-hvwt[indx-1])+(hvwt[indx+v1]));

					}
			for(rr=12; rr<rr1-12; rr++)
				for(cc=12,indx=rr*TS+cc; cc<cc1-12; cc++,indx++){
					rgb[indx][0]=(rgb[indx][1]-Dgrb[indx][0]);
					rgb[indx][2]=(rgb[indx][1]-Dgrb[indx][1]);
				}
			//t2_chroma += clock() - t1_chroma;

			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
			// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

			// copy smoothed results back to image matrix
			for (rr=16; rr < rr1-16; rr++)
				for (row=rr+top, cc=16; cc < cc1-16; cc++) {
					col = cc + left;

					indx=rr*TS+cc;

					//red[row][col] = ((65535.0f*rgb[indx][0] ));
					//green[row][col] = ((65535.0f*rgb[indx][1]));
					//blue[row][col] = ((65535.0f*rgb[indx][2]));
					out[(row*width+col)*4]   = rgb[indx][0];
					out[(row*width+col)*4+1] = rgb[indx][1];
					out[(row*width+col)*4+2] = rgb[indx][2];

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
