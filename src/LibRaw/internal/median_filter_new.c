/* This file was taken from modified dcraw published by Paul Lee
   on January 23, 2009, taking dcraw ver.8.90/rev.1.417
   as basis.
   http://sites.google.com/site/demosaicalgorithms/modified-dcraw

   As modified dcraw source code was published, the release under
   GPL Version 2 or later option could be applied, so this file 
   is taken under this premise.
*/

/*
    Copyright (C) 2009 Paul Lee

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


/* 
   differential median filter 
*/
#define PIX_SORT(a,b) { if ((a)>(b)) {temp=(a);(a)=(b);(b)=temp;} }
void CLASS median_filter_new()
{
  int (*mf)[3], (*pc)[3], p[9], indx, row, col, c, d, temp, i, v0, pass;
  int p11, p12, p13, p31, p32, p33;
  p11 = -width-1;
  p12 = p11+1;
  p13 = p12+1;
  p31 =  width-1;
  p32 = p31+1;
  p33 = p32+1;
  /* Allocate buffer for 3x3 median filter */
  mf = (int (*)[3]) calloc(width*height, sizeof *mf);
  for (pass=1; pass <= med_passes; pass++) {
    if (verbose)
      fprintf (stderr,_("3x3 differential median filter pass %d...\n"), pass);
    for (c=0; c < 3; c+=2) {
      /* Compute median(R-G) and median(B-G) */ 
      for (indx=0; indx < height*width; indx++)
	mf[indx][c] = image[indx][c] - image[indx][1];
      /* Apply 3x3 median fileter */
#pragma omp parallel for private(row,col,pc)

      for (row=1; row < height-1; row++)
	for (col=1; col < width-1; col++) {
	  pc = mf + row*width+col;
	  /* Assign 3x3 differential color values */
	  p[0] = pc[p11][c]; p[1] = pc[p12][c]; p[2] = pc[p13][c];
	  p[3] = pc[ -1][c]; p[4] = pc[  0][c]; p[5] = pc[  1][c];
	  p[6] = pc[p31][c]; p[7] = pc[p32][c]; p[8] = pc[p33][c];
	  /* Sort for median of 9 values */
	  PIX_SORT(p[1],p[2]); PIX_SORT(p[4], p[5]); PIX_SORT(p[7],p[8]);
	  PIX_SORT(p[0],p[1]); PIX_SORT(p[3], p[4]); PIX_SORT(p[6],p[7]);
	  PIX_SORT(p[1],p[2]); PIX_SORT(p[4], p[5]); PIX_SORT(p[7],p[8]);
	  PIX_SORT(p[0],p[3]); PIX_SORT(p[5], p[8]); PIX_SORT(p[4],p[7]);
	  PIX_SORT(p[3],p[6]); PIX_SORT(p[1], p[4]); PIX_SORT(p[2],p[5]);
	  PIX_SORT(p[4],p[7]); PIX_SORT(p[4], p[2]); PIX_SORT(p[6],p[4]);
	  PIX_SORT(p[4],p[2]);
	  pc[0][1] = p[4];
	}
      for (row=1; row < height-1; row++)
	for (col=1; col < width-1; col++) {
	  pc = mf + row*width+col;
          pc[0][c] = pc[0][1]; }
    }

    /* red/blue at GREEN pixel locations */
    for (row=1; row < height-1; row++)
      for (col=1+(FC(row,2) & 1), c=FC(row,col+1); col < width-1; col+=2) {
	indx = row*width+col;
	for (i=0; i < 2; c=2-c, i++) {
	  v0 = image[indx][1]+mf[indx][c];
	  image[indx][c] = CLIP(v0);
	}
      }

    /* red/blue at BLUE/RED pixel locations */
    for (row=2; row < height-2; row++)
      for (col=2+(FC(row,2) & 1), c=2-FC(row,col); col < width-2; col+=2) {
	indx = row*width+col;
	v0 = image[indx][1]+mf[indx][c];
	image[indx][c] = CLIP(v0);
      }

    /* green at RED/BLUE location */
    for (row=1; row < height-1; row++)
      for (col=1+(FC(row,1) & 1), c=FC(row,col); col < width-3; col+=2) {
	indx = row*width+col;
	d = 2 - c;
	v0 = (image[indx][c]-mf[indx][c]+image[indx][d]-mf[indx][d]+1) >> 1;
	image[indx][1] = CLIP(v0);
      }
  }

  /* Free buffer */
  free(mf);
}
#undef PIX_SORT
