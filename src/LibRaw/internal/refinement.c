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
   Refinement based on EECI demosaicing algorithm by L. Chang and Y.P. Tan
*/
void CLASS refinement()
{
  ushort (*pix)[4];
  int row, col, indx, c, i, d, w1, w2, v0;
  double dL, dR, dU, dD, dt;
  clock_t t1, t2;

  if (verbose) fprintf(stderr,_("Post-demosaic refinement..."));
  t1 = clock();
  w1 = width;
  w2 = 2*w1;

  /* Reinforce interpolated green pixels on RED/BLUE pixel locations */
  for (row=2; row < height-2; row++)
    for (col=2+(FC(row,2) & 1), c=FC(row,col); col < width-2; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      dL = 1.0/(1.0+ABS(pix[ -2][c]-pix[0][c])+ABS(pix[ 1][1]-pix[ -1][1]));
      dR = 1.0/(1.0+ABS(pix[  2][c]-pix[0][c])+ABS(pix[ 1][1]-pix[ -1][1]));
      dU = 1.0/(1.0+ABS(pix[-w2][c]-pix[0][c])+ABS(pix[w1][1]-pix[-w1][1]));
      dD = 1.0/(1.0+ABS(pix[ w2][c]-pix[0][c])+ABS(pix[w1][1]-pix[-w1][1]));
      v0 = (int)((double)pix[0][c] + 0.5 +
		 ((double)(pix[ -1][1]-pix[ -1][c])*dL +
		  (double)(pix[  1][1]-pix[  1][c])*dR +
		  (double)(pix[-w1][1]-pix[-w1][c])*dU +
		  (double)(pix[ w1][1]-pix[ w1][c])*dD ) / (dL+dR+dU+dD));
      pix[0][1] = CLIP(v0);
    }

  /* Reinforce interpolated red/blue pixels on GREEN pixel locations */
  for (row=2; row < height-2; row++)
    for (col=2+(FC(row,3) & 1), c=FC(row,col+1); col < width-2; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      for (i=0; i < 2; c=2-c, i++) {
	dL = 1.0/(1.0+ABS(pix[ -2][1]-pix[0][1])+ABS(pix[ 1][c]-pix[ -1][c]));
	dR = 1.0/(1.0+ABS(pix[  2][1]-pix[0][1])+ABS(pix[ 1][c]-pix[ -1][c]));
	dU = 1.0/(1.0+ABS(pix[-w2][1]-pix[0][1])+ABS(pix[w1][c]-pix[-w1][c]));
	dD = 1.0/(1.0+ABS(pix[ w2][1]-pix[0][1])+ABS(pix[w1][c]-pix[-w1][c]));
	v0 = (int)((double)pix[0][1] + 0.5 -
		   ((double)(pix[ -1][1]-pix[ -1][c])*dL +
		    (double)(pix[  1][1]-pix[  1][c])*dR +
		    (double)(pix[-w1][1]-pix[-w1][c])*dU +
		    (double)(pix[ w1][1]-pix[ w1][c])*dD ) / (dL+dR+dU+dD));
	pix[0][c] = CLIP(v0);
      }
    }

  /* Reinforce integrated red/blue pixels on BLUE/RED pixel locations */
  for (row=2; row < height-2; row++)
    for (col=2+(FC(row,2) & 1), c=2-FC(row,col); col < width-2; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      d = 2 - c;
      dL = 1.0/(1.0+ABS(pix[ -2][d]-pix[0][d])+ABS(pix[ 1][1]-pix[ -1][1]));
      dR = 1.0/(1.0+ABS(pix[  2][d]-pix[0][d])+ABS(pix[ 1][1]-pix[ -1][1]));
      dU = 1.0/(1.0+ABS(pix[-w2][d]-pix[0][d])+ABS(pix[w1][1]-pix[-w1][1]));
      dD = 1.0/(1.0+ABS(pix[ w2][d]-pix[0][d])+ABS(pix[w1][1]-pix[-w1][1]));
      v0 = (int)((double)pix[0][1] + 0.5 -
		 ((double)(pix[ -1][1]-pix[ -1][c])*dL +
		  (double)(pix[  1][1]-pix[  1][c])*dR +
		  (double)(pix[-w1][1]-pix[-w1][c])*dU +
		  (double)(pix[ w1][1]-pix[ w1][c])*dD ) / (dL+dR+dU+dD));
      pix[0][c] = CLIP(v0);
    }
  
  /* Done */
  t2 = clock();
  dt = ((double)(t2-t1)) / CLOCKS_PER_SEC;
  if (verbose) fprintf(stderr,_("\telapsed time     = %5.3fs\n"),dt);
}
