/* 
   Color Demosaicing Using Variance of Color Differences
   by K.H Chung and Y.H Chan 
*/
#define PIX_SORT(a,b) { if ((a)>(b)) {temp=(a);(a)=(b);(b)=temp;} }
void CLASS vcd_interpolate(int ahd_cutoff)
{
  int row, col, indx, c, d, i, j;
  ushort (*pix)[4];
  int w1, w2, w3, w4, w5, w6, v0, v1, v2, v3, v4, LH, LV, var_dir;
  int AHD_cnt, LH_cnt, LV_cnt, varH_cnt, varV_cnt, varD_cnt, T_cnt;
  int dC0, dC1, dC2, dC3, dC4, temp;
  double e, T, d1, d3, d5, d7, d9, varH, varV, varD, var0;
  clock_t t1, t2;
  double dt;
  if (verbose) fprintf(stderr,_("VCD interpolation...\n"));
  t1 = clock();
  /* assume VCD's T value is based on gamma=2.22 test images */
  T = 2;
  AHD_cnt = LH_cnt = LV_cnt = varH_cnt = varV_cnt = varD_cnt = 0;
  w1 = width;
  w2 = 2*w1;
  w3 = 3*w1;
  w4 = 4*w1;
  w5 = 5*w1;
  w6 = 6*w1;
  border_interpolate(6);

  /* run AHD interpolation on Green channel with threshold */
  if (ahd_cutoff > 0) ahd_partial_interpolate(ahd_cutoff);

  /* Interpolate green pixels on red/blue pixel locations */  
  for (row=6; row < height-6; row++)
    for (col=6+(FC(row,6) & 1), c=FC(row,col); col < width-6; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      d = 2 - c;
      if (image[indx][1] > 0) {
	var_dir = 4;
	AHD_cnt++; }
      else {
	/* LH: Eq.(6) */
	LH = 
	  ABS(pix[-2-w2][c]-pix[-w2][c]) + ABS(pix[-w2][c]-pix[2-w2][c]) +
	  ABS(pix[-2   ][c]-pix[  0][c]) + ABS(pix[  0][c]-pix[2   ][c]) +
	  ABS(pix[-2+w2][c]-pix[ w2][c]) + ABS(pix[ w2][c]-pix[2+w2][c]) +
	  ABS(pix[-2-w1][1]-pix[-w1][1]) + ABS(pix[-w1][1]-pix[2-w1][1]) +
	  ABS(pix[-2+w1][1]-pix[ w1][1]) + ABS(pix[ w1][1]-pix[2+w1][1]) +
	  ABS(pix[-1-w2][1]-pix[-w2][c]) + ABS(pix[-w2][c]-pix[1-w2][1]) +
	  ABS(pix[-1   ][1]-pix[  0][c]) + ABS(pix[  0][c]-pix[1   ][1]) +
	  ABS(pix[-1+w2][1]-pix[ w2][c]) + ABS(pix[ w2][c]-pix[1+w2][1]) +
	  ABS(pix[-1-w1][d]-pix[-w1][1]) + ABS(pix[-w1][1]-pix[1-w1][d]) +
	  ABS(pix[-1+w1][d]-pix[ w1][1]) + ABS(pix[ w1][1]-pix[1+w1][d]);
	/* LV: Eq.(7) */
	LV =
	  ABS(pix[-2-w2][c]-pix[-2][c]) + ABS(pix[-2][c]-pix[-2+w2][c]) +
	  ABS(pix[  -w2][c]-pix[ 0][c]) + ABS(pix[ 0][c]-pix[   w2][c]) +
	  ABS(pix[ 2-w2][c]-pix[ 2][c]) + ABS(pix[ 2][c]-pix[ 2+w2][c]) +
	  ABS(pix[-1-w2][1]-pix[-1][1]) + ABS(pix[-1][1]-pix[-1+w2][1]) +
	  ABS(pix[ 1-w2][1]-pix[ 1][1]) + ABS(pix[ 1][1]-pix[ 1+w2][1]) +
	  ABS(pix[-2-w1][1]-pix[-2][c]) + ABS(pix[-2][c]-pix[-2+w1][1]) +
	  ABS(pix[  -w1][1]-pix[ 0][c]) + ABS(pix[ 0][c]-pix[   w1][1]) +
	  ABS(pix[ 2-w1][1]-pix[ 2][c]) + ABS(pix[ 2][c]-pix[ 2+w1][1]) +
	  ABS(pix[-1-w1][d]-pix[-1][1]) + ABS(pix[-1][1]-pix[-1+w1][d]) +
	  ABS(pix[ 1-w1][d]-pix[ 1][1]) + ABS(pix[ 1][1]-pix[ 1+w1][d]);
	/* e: Eq.(8) */
	e = (double)(LH) / (double)(LV);
	if (e < 1) e = 1.0/e;
	/* g: Eq.(9)~(10) */
	if (e > T) {
	  if (LH < LV) {
	    var_dir = 1;
	    LH_cnt++; }
	  else {
	    var_dir = 2;
	    LV_cnt++; }
	}
	/* varH, varV, varD: Eq. (11)~(18) */
	else {
	  /* varH: Eq.(11) */
	  d1 = (double)(pix[-6][c]-2*(pix[-5][1]-pix[-4][c]+pix[-3][1])+pix[-2][c])/65535.0;
	  d3 = (double)(pix[-4][c]-2*(pix[-3][1]-pix[-2][c]+pix[-1][1])+pix[ 0][c])/65535.0;
	  d5 = (double)(pix[-2][c]-2*(pix[-1][1]-pix[ 0][c]+pix[ 1][1])+pix[ 2][c])/65535.0;
	  d7 = (double)(pix[ 0][c]-2*(pix[ 1][1]-pix[ 2][c]+pix[ 3][1])+pix[ 4][c])/65535.0;
	  d9 = (double)(pix[ 2][c]-2*(pix[ 3][1]-pix[ 4][c]+pix[ 5][1])+pix[ 6][c])/65535.0;
	  /* variance assuming d2 =(d1+d3)/2, d4=(d3+d5)/2, and etc */
	  varH =
	    d1*(18*d1 - 3*d3 - 12*d5 - 12*d7 - 9*d9) +
	    d3*(19*d3 - 7*d5 - 16*d7 - 12*d9) +
	    d5*(19*d5 - 7*d7 - 12*d9) +
	    d7*(19*d7 - 3*d9) +
	    18*d9*d9;
	  var_dir = 1;
	  var0 = varH;
	  /* varV: Eq.(12) */
	  d1 = (double)(pix[-w6][c]-2*(pix[-w5][1]-pix[-w4][c]+pix[-w3][1])+pix[-w2][c])/65535.0;
	  d3 = (double)(pix[-w4][c]-2*(pix[-w3][1]-pix[-w2][c]+pix[-w1][1])+pix[  0][c])/65535.0;
	  d5 = (double)(pix[-w2][c]-2*(pix[-w1][1]-pix[  0][c]+pix[ w1][1])+pix[ w2][c])/65535.0;
	  d7 = (double)(pix[  0][c]-2*(pix[ w1][1]-pix[ w2][c]+pix[ w3][1])+pix[ w4][c])/65535.0;
	  d9 = (double)(pix[ w2][c]-2*(pix[ w3][1]-pix[ w4][c]+pix[ w5][1])+pix[ w6][c])/65535.0;
	  varV =
	    d1*(18*d1 - 3*d3 - 12*d5 - 12*d7 - 9*d9) +
	    d3*(19*d3 - 7*d5 - 16*d7 - 12*d9) +
	    d5*(19*d5 - 7*d7 - 12*d9) +
	    d7*(19*d7 - 3*d9) +
	    18*d9*d9;
	  if (varV < var0) {
	    var_dir = 2;
	    var0 = varV; }
	  /* varD: Eq.(17) */
	  d1 = (double)(4*pix[-4   ][c]+pix[-4-w2][c]+pix[-6][c]+pix[-2   ][c]+pix[-4+w2][c]
        	      -2*(pix[-4-w1][1]+pix[-5   ][1]+pix[-3][1]+pix[-4+w1][1]))/65535.0;	
	  d3 = (double)(4*pix[-2   ][c]+pix[-2-w2][c]+pix[-4][c]+pix[ 0   ][c]+pix[-2+w2][c]
		      -2*(pix[-2-w1][1]+pix[-3   ][1]+pix[-1][1]+pix[-2+w1][1]))/65535.0;	
	  d5 = (double)(4*pix[ 0   ][c]+pix[  -w2][c]+pix[-2][c]+pix[ 2   ][c]+pix[   w2][c]
		      -2*(pix[  -w1][1]+pix[-1   ][1]+pix[ 1][1]+pix[   w1][1]))/65535.0;	
	  d7 = (double)(4*pix[ 2   ][c]+pix[ 2-w2][c]+pix[ 0][c]+pix[ 4   ][c]+pix[ 2+w2][c]
		      -2*(pix[ 2-w1][1]+pix[ 1   ][1]+pix[ 3][1]+pix[ 2+w1][1]))/65535.0;	
	  d9 = (double)(4*pix[ 4   ][c]+pix[ 4-w2][c]+pix[ 2][c]+pix[ 6   ][c]+pix[ 4+w2][c]
		      -2*(pix[ 4-w1][1]+pix[ 3   ][1]+pix[ 5][1]+pix[ 4+w1][1]))/65535.0;
	  varD =
	    d1*(18*d1 - 3*d3 - 12*d5 - 12*d7 - 9*d9) +
	    d3*(19*d3 - 7*d5 - 16*d7 - 12*d9) +
	    d5*(19*d5 - 7*d7 - 12*d9) +
	    d7*(19*d7 - 3*d9) +
	    18*d9*d9;
	  /* d5 stays same */
	  d1 = (double)(4*pix[-w4][c]+pix[-w6  ][c]+pix[-w4-2][c]+pix[-w4+2][c]+pix[-w2][c]
		      -2*(pix[-w5][1]+pix[-w4-1][1]+pix[-w4+1][1]+pix[-w3  ][1]))/65535.0;
	  d3 = (double)(4*pix[-w2][c]+pix[-w4  ][c]+pix[-w2-2][c]+pix[-w2+2][c]+pix[  0][c]
		      -2*(pix[-w3][1]+pix[-w2-1][1]+pix[-w2+1][1]+pix[-w1  ][1]))/65535.0;
	  d7 = (double)(4*pix[ w2][c]+pix[    0][c]+pix[ w2-2][c]+pix[ w2+2][c]+pix[ w4][c]
		      -2*(pix[ w1][1]+pix[ w2-1][1]+pix[ w2+1][1]+pix[ w3  ][1]))/65535.0;
	  d9 = (double)(4*pix[ w4][c]+pix[ w2  ][c]+pix[ w4-2][c]+pix[ w4+2][c]+pix[ w6][c]
		      -2*(pix[ w3][1]+pix[ w4-1][1]+pix[ w4+1][1]+pix[ w5  ][1]))/65535.0;
	  varD +=
	    d1*(18*d1 - 3*d3 - 12*d5 - 12*d7 - 9*d9) +
	    d3*(19*d3 - 7*d5 - 16*d7 - 12*d9) +
	    d5*(19*d5 - 7*d7 - 12*d9) +
	    d7*(19*d7 - 3*d9) +
	    18*d9*d9;
	  /* scale varD to equalize to varH and varV */
	  varD /= 8.0;
	  if (varD < var0) var_dir = 3;
	  /* Eq.(18) */
	  if (var_dir == 1)
	    varH_cnt++;
	  else if (var_dir == 2)
	    varV_cnt++;
	  else if (var_dir == 3)
	    varD_cnt++;
	}
      }
      /* limit values within surrounding green values 
	 for overshooted pixel values, revert to linear interpolation */
      if (var_dir == 1) {
	/* Eq.(3) - Horizontal */
	v0 = 
	  (2*(pix[ -1][1]+pix[0][c]+pix[ 1][1])-pix[ -2][c]-pix[ 2][c]+2) >> 2;
	v1 = pix[-1][1];
	v2 = pix[ 1][1];
	PIX_SORT(v1,v2);
	/* +- 50% range */
	v3 = MAX(2*v1 - v2,0);
	v4 = MIN(2*v2 - v1,65535);
	if (v0 < v3 || v0 > v4) {
	  v0 = (pix[-3][1] + pix[3][1] +
		18*(2*pix[0][c] - pix[-2][c] - pix[2][c]) +
		63*(pix[-1][1] + pix[1][1]) + 64) >> 7;
	  if (v0 < v3 || v0 > v4) {
	    v0 = (4*(v1 + v2) + 2*pix[0][c]-pix[-2][c]-pix[2][c] + 4) >> 3;
	    /* Bi-linear if anti-aliasing overshoots */
	    if (v0 < v3 || v0 > v4) v0 = (v1 + v2 + 1) >> 1; } }
      }
      else if (var_dir == 2) {
	/* Eq.(4) - Vertical */
	v0 =
	  (2*(pix[-w1][1]+pix[0][c]+pix[w1][1])-pix[-w2][c]-pix[w2][c]+2) >> 2;
	v1 = pix[-w1][1];
	v2 = pix[ w1][1];
	PIX_SORT(v1,v2);
	/* +- 50% range */
	v3 = MAX(2*v1 - v2,0);
	v4 = MIN(2*v2 - v1,65535);
	if (v0 < v3 || v0 > v4) {
	  v0 = (pix[-w3][1] + pix[w3][1] +
		18*(2*pix[0][c] - pix[-w2][c] - pix[w2][c]) +
		63*(pix[-w1][1] + pix[w1][1]) + 64) >> 7;
	  if (v0 < v3 || v0 > v4) {
	    v0 = (4*(v1 + v2) + 2*pix[0][c]-pix[-w2][c]-pix[w2][c] + 4) >> 3;
	    /* Bi-linear if anti-aliasing overshoots */
	    if (v0 < v3 || v0 > v4) v0 = (v1 + v2 + 1) >> 1; }}
      }
      else if (var_dir == 3) {
	/* Eq.(5) - Diagonal */
	v0  = 2*(pix[ -1][1]+pix[0][c]+pix[ 1][1])-pix[ -2][c]-pix[ 2][c]+2;
	v0 += 2*(pix[-w1][1]+pix[0][c]+pix[w1][1])-pix[-w2][c]-pix[w2][c]+2;
	v0 >>= 3;
	v1 = MIN(MIN(pix[-1][1],pix[1][1]),MIN(pix[-w1][1],pix[w1][1]));
	v2 = MAX(MAX(pix[-1][1],pix[1][1]),MAX(pix[-w1][1],pix[w1][1]));
	v3 = MAX(2*v1 - v2,0);
	v4 = MIN(2*v2 - v1,65535);
	if (v0 < v3 || v0 > v4)
	  v0 = (pix[-w1][1] + pix[ -1][1] + pix[ 1][1] + pix[w1][1] + 2) >> 2; 
      } 
      else if (var_dir == 4) {
	v0 = pix[0][1];
      }
      pix[0][1] = v0;
    }

  /*
     Interpolote red/blue pixels on BLUE/RED pixel locations
     using pattern regcognition on differential color plane
  */
  for (row=1; row < height-1; row++)
    for (col=1+(FC(row,1) & 1), c=2-FC(row,col); col < width-1; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      if (pix[0][c] == 0) {
	dC1 = pix[-w1-1][1]-pix[-w1-1][c];
	dC2 = pix[-w1+1][1]-pix[-w1+1][c];
	dC3 = pix[ w1-1][1]-pix[ w1-1][c];
	dC4 = pix[ w1+1][1]-pix[ w1+1][c];
	dC0 = dC1 + dC2 + dC3 + dC4;
	dC1 <<= 2;
	dC2 <<= 2;
	dC3 <<= 2;
	dC4 <<= 2;
	j = (dC1 > dC0) + (dC2 > dC0) + (dC3 > dC0) + (dC4 > dC0);
	if (j == 3 || j == 1) {
	  /* edge-corner pattern:  median of color differential values */
	  PIX_SORT(dC1,dC2);
	  PIX_SORT(dC3,dC4);
	  PIX_SORT(dC1,dC3);
	  PIX_SORT(dC2,dC4);
	  dC0 = dC2 + dC3; }
	else {
	  /* stripe pattern: average along diagonal */
	  v1 = ABS(pix[-w1-1][c]-pix[w1+1][c]);
	  v2 = ABS(pix[-w1+1][c]-pix[w1-1][c]);
	  if (v1 < v2)
	    dC0 = dC1 + dC4;
	  else if (v1 > v2)
	    dC0 = dC2 + dC3;
	  else
	    dC0 <<= 1; }
	v0 = (((int)(pix[0][1]) << 3) - dC0 + 4) >> 3;
	/* apply anti-aliasing if overshoot */
	if (v0 < 0 || v0 > 65535)
	  v0 = (pix[-w1-1][c]+pix[-w1+1][c]+pix[w1-1][c]+pix[w1+1][c]+2) >> 2;
	pix[0][c] = v0;
      }
    }
  /* 
     Interpolote red/blue pixels on GREEN pixel locations
     using pattern regcognition on differential color plane
  */
  for (row=1; row < height-1; row++)
    for (col=1+(FC(row,2) & 1), c=FC(row,col+1); col < width-1; col+=2) {
      indx = row*width+col;
      pix = image + indx;
      for (i=0; i < 2; c=2-c, i++) {
	if (pix[0][c] == 0) {
	  dC1 = pix[-w1][1]-pix[-w1][c];
	  dC2 = pix[ -1][1]-pix[ -1][c];
	  dC3 = pix[  1][1]-pix[  1][c];
	  dC4 = pix[ w1][1]-pix[ w1][c];
	  dC0 = dC1 + dC2 + dC3 + dC4;
	  dC1 <<= 2;
	  dC2 <<= 2;
	  dC3 <<= 2;
	  dC4 <<= 2;
	  j = (dC1 > dC0) + (dC2 > dC0) + (dC3 > dC0) + (dC4 > dC0);
	  if (j == 3 || j == 1) {
	    /* edge-corner pattern:  median of color differential values */
	    PIX_SORT(dC1,dC2);
	    PIX_SORT(dC3,dC4);
	    PIX_SORT(dC1,dC3);
	    PIX_SORT(dC2,dC4);
	    dC0 = dC2 + dC3; }
	  else {
	    /* stripe pattern: average along diagonal */
	    v1 = ABS(pix[-w1][c]-pix[w1][c]);
	    v2 = ABS(pix[ -1][c]-pix[ 1][c]);
	    if (v1 < v2)
	      dC0 = dC1 + dC4;
	    else if (v1 > v2)
	      dC0 = dC2 + dC3;
	    else
	      dC0 <<= 1; }
	  v0 = (((int)(pix[0][1]) << 3) - dC0 + 4) >> 3;
	  /* apply anti-aliasing if overshoot */
	  if (v0 < 0 || v0 > 65535) {
	    if (i == 0)
	      v0 = (pix[ -1][c]+pix[ 1][c]+1) >> 1;
	    else
	      v0 = (pix[-w1][c]+pix[w1][c]+1) >> 1; }
	  pix[0][c] = v0;
	}
      }
    }
  
  /* Compute statistics */
  if (verbose) {
    if (ahd_cutoff > 0) {
      T_cnt = AHD_cnt + LH_cnt + LV_cnt + varH_cnt + varV_cnt + varD_cnt;
      fprintf (stderr,
	       _("\tAHD, LH, LV, varH, varV varD = %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f (%%)\n"),
	       100*(double)AHD_cnt/(double)T_cnt,
	       100*(double)LH_cnt/(double)T_cnt,
	       100*(double)LV_cnt/(double)T_cnt,
	       100*(double)varH_cnt/(double)T_cnt,
	       100*(double)varV_cnt/(double)T_cnt,
	       100*(double)varD_cnt/(double)T_cnt); }
    else {
      T_cnt = LH_cnt + LV_cnt + varH_cnt + varV_cnt + varD_cnt;
      fprintf (stderr,
	       _("\tLH, LV, varH, varV varD = %4.2f, %4.2f, %4.2f, %4.2f, %4.2f (%%)\n"),
	       100*(double)LH_cnt/(double)T_cnt,
	       100*(double)LV_cnt/(double)T_cnt,
	       100*(double)varH_cnt/(double)T_cnt,
	       100*(double)varV_cnt/(double)T_cnt,
	       100*(double)varD_cnt/(double)T_cnt); }
  }
  
  /* Done */
  t2 = clock();
  dt = ((double)(t2-t1)) / CLOCKS_PER_SEC;
  if (verbose) fprintf(stderr,_("\telapsed time     = %5.3fs\n"),dt);
}
#undef PIX_SORT
