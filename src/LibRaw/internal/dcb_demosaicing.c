/*
 *      Redistribution and use in source and binary forms, with or without
 *      modification, are permitted provided that the following conditions are
 *      met:
 *      
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following disclaimer
 *        in the documentation and/or other materials provided with the
 *        distribution.
 *      * Neither the name of the author nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *      
 *      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *      "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *      LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *      A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *      OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *      SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *      LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *      DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *      THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *      (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *      OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// If you want to use the code, you need to display name of the original authors in
// your software!
  


/* DCB demosaicing by Jacek Gozdz (cuniek@kft.umcs.lublin.pl)
 * the implementation is not speed optimised
 * the code is open source (BSD licence)
*/

/* FBDD denoising by Jacek Gozdz (cuniek@kft.umcs.lublin.pl) and Luis Sanz Rodríguez (luis.sanz.rodriguez@gmail.com)
 * the implementation is not speed optimised
 * the code is open source (BSD licence)
*/



 
// R and B smoothing using green contrast, all pixels except 2 pixel wide border
void CLASS dcb_pp()
{
	int g1, r1, b1, u=width, indx, row, col;

	
	for (row=2; row < height-2; row++) 
	for (col=2, indx=row*u+col; col < width-2; col++, indx++) { 

		r1 = ( image[indx-1][0] + image[indx+1][0] + image[indx-u][0] + image[indx+u][0] + image[indx-u-1][0] + image[indx+u+1][0] + image[indx-u+1][0] + image[indx+u-1][0])/8.0;
		g1 = ( image[indx-1][1] + image[indx+1][1] + image[indx-u][1] + image[indx+u][1] + image[indx-u-1][1] + image[indx+u+1][1] + image[indx-u+1][1] + image[indx+u-1][1])/8.0;
		b1 = ( image[indx-1][2] + image[indx+1][2] + image[indx-u][2] + image[indx+u][2] + image[indx-u-1][2] + image[indx+u+1][2] + image[indx-u+1][2] + image[indx+u-1][2])/8.0;
		 
		image[indx][0] = CLIP(r1 + ( image[indx][1] - g1 ));
		image[indx][2] = CLIP(b1 + ( image[indx][1] - g1 ));
	
	}
}




// saves red and blue
void CLASS copy_to_buffer(float (*image2)[3])
{
	int indx;

	for (indx=0; indx < height*width; indx++) {
		image2[indx][0]=image[indx][0]; //R
		image2[indx][2]=image[indx][2]; //B
	}
}



// restores red and blue
void CLASS restore_from_buffer(float (*image2)[3])
{
	int indx;

	for (indx=0; indx < height*width; indx++) {
		image[indx][0]=image2[indx][0]; //R
		image[indx][2]=image2[indx][2]; //B
	}
}



// fast green interpolation
void CLASS hid()
{
	int row, col, c, u=width, v=2*u, indx;
	
	for (row=2; row < height-2; row++) {
	for (col=2, indx=row*width+col; col < width-2; col++, indx++) { 	

		c =  fc(row,col);
		if(c != 1) 
		{
			image[indx][1] = CLIP((image[indx+u][1] + image[indx-u][1] + image[indx-1][1] + image[indx+1][1])/4.0 + 
							 (image[indx][c] - ( image[indx+v][c] + image[indx-v][c] + image[indx-2][c] + image[indx+2][c])/4.0)/2.0);
		}
	} 
	}	
	
}


// green correction
void CLASS hid2()
{
	int row, col, c, u=width, v=2*u, indx;

	
	for (row=4; row < height-4; row++) {
	for (col=4, indx=row*width+col; col < width-4; col++, indx++) { 	

		c =  fc(row,col);

		if (c != 1)
		{	
			image[indx][1] = CLIP((image[indx+v][1] + image[indx-v][1] + image[indx-2][1] + image[indx+2][1])/4.0 + 
							  image[indx][c] - ( image[indx+v][c] + image[indx-v][c] + image[indx-2][c] + image[indx+2][c])/4.0);
    	}	

	} 
	}	

}





// missing colors are interpolated
void CLASS dcb_color()
{
	int row, col, c, d, u=width, indx;


	for (row=1; row < height-1; row++)
		for (col=1+(FC(row,1) & 1), indx=row*width+col, c=2-FC(row,col); col < u-1; col+=2, indx+=2) {

			
			image[indx][c] = CLIP(( 
			4*image[indx][1] 
			- image[indx+u+1][1] - image[indx+u-1][1] - image[indx-u+1][1] - image[indx-u-1][1] 
			+ image[indx+u+1][c] + image[indx+u-1][c] + image[indx-u+1][c] + image[indx-u-1][c] )/4.0);
		}

	for (row=1; row<height-1; row++)
		for (col=1+(FC(row,2)&1), indx=row*width+col,c=FC(row,col+1),d=2-c; col<width-1; col+=2, indx+=2) {
			
			image[indx][c] = CLIP((2*image[indx][1] - image[indx+1][1] - image[indx-1][1] + image[indx+1][c] + image[indx-1][c])/2.0);
			image[indx][d] = CLIP((2*image[indx][1] - image[indx+u][1] - image[indx-u][1] + image[indx+u][d] + image[indx-u][d])/2.0);
		}	
}


// missing colors are interpolated using high quality algorithm by Luis Sanz Rodríguez
void CLASS dcb_color_full()
{
	int row,col,c,d,i,j,u=width,v=2*u,w=3*u,indx;
	float f[4],g[4],(*chroma)[2];

	chroma = (float (*)[2]) calloc(width*height,sizeof *chroma); merror (chroma, "dcb_color_full()");

	for (row=1; row < height-1; row++)
		for (col=1+(FC(row,1)&1),indx=row*width+col,c=FC(row,col),d=c/2; col < u-1; col+=2,indx+=2)
			chroma[indx][d]=image[indx][c]-image[indx][1];

	for (row=3; row<height-3; row++)
		for (col=3+(FC(row,1)&1),indx=row*width+col,c=1-FC(row,col)/2,d=1-c; col<u-3; col+=2,indx+=2) {
			f[0]=1.0/(float)(1.0+fabs(chroma[indx-u-1][c]-chroma[indx+u+1][c])+fabs(chroma[indx-u-1][c]-chroma[indx-w-3][c])+fabs(chroma[indx+u+1][c]-chroma[indx-w-3][c]));
			f[1]=1.0/(float)(1.0+fabs(chroma[indx-u+1][c]-chroma[indx+u-1][c])+fabs(chroma[indx-u+1][c]-chroma[indx-w+3][c])+fabs(chroma[indx+u-1][c]-chroma[indx-w+3][c]));
			f[2]=1.0/(float)(1.0+fabs(chroma[indx+u-1][c]-chroma[indx-u+1][c])+fabs(chroma[indx+u-1][c]-chroma[indx+w+3][c])+fabs(chroma[indx-u+1][c]-chroma[indx+w-3][c]));
			f[3]=1.0/(float)(1.0+fabs(chroma[indx+u+1][c]-chroma[indx-u-1][c])+fabs(chroma[indx+u+1][c]-chroma[indx+w-3][c])+fabs(chroma[indx-u-1][c]-chroma[indx+w+3][c]));
			g[0]=1.325*chroma[indx-u-1][c]-0.175*chroma[indx-w-3][c]-0.075*chroma[indx-w-1][c]-0.075*chroma[indx-u-3][c];
			g[1]=1.325*chroma[indx-u+1][c]-0.175*chroma[indx-w+3][c]-0.075*chroma[indx-w+1][c]-0.075*chroma[indx-u+3][c];
			g[2]=1.325*chroma[indx+u-1][c]-0.175*chroma[indx+w-3][c]-0.075*chroma[indx+w-1][c]-0.075*chroma[indx+u-3][c];
			g[3]=1.325*chroma[indx+u+1][c]-0.175*chroma[indx+w+3][c]-0.075*chroma[indx+w+1][c]-0.075*chroma[indx+u+3][c];
			chroma[indx][c]=(f[0]*g[0]+f[1]*g[1]+f[2]*g[2]+f[3]*g[3])/(f[0]+f[1]+f[2]+f[3]);
		}
	for (row=3; row<height-3; row++)
		for (col=3+(FC(row,2)&1),indx=row*width+col,c=FC(row,col+1)/2; col<u-3; col+=2,indx+=2)
			for(d=0;d<=1;c=1-c,d++){
				f[0]=1.0/(float)(1.0+fabs(chroma[indx-u][c]-chroma[indx+u][c])+fabs(chroma[indx-u][c]-chroma[indx-w][c])+fabs(chroma[indx+u][c]-chroma[indx-w][c]));
				f[1]=1.0/(float)(1.0+fabs(chroma[indx+1][c]-chroma[indx-1][c])+fabs(chroma[indx+1][c]-chroma[indx+3][c])+fabs(chroma[indx-1][c]-chroma[indx+3][c]));
				f[2]=1.0/(float)(1.0+fabs(chroma[indx-1][c]-chroma[indx+1][c])+fabs(chroma[indx-1][c]-chroma[indx-3][c])+fabs(chroma[indx+1][c]-chroma[indx-3][c]));
				f[3]=1.0/(float)(1.0+fabs(chroma[indx+u][c]-chroma[indx-u][c])+fabs(chroma[indx+u][c]-chroma[indx+w][c])+fabs(chroma[indx-u][c]-chroma[indx+w][c]));
			
				g[0]=0.875*chroma[indx-u][c]+0.125*chroma[indx-w][c];
				g[1]=0.875*chroma[indx+1][c]+0.125*chroma[indx+3][c];
				g[2]=0.875*chroma[indx-1][c]+0.125*chroma[indx-3][c];
				g[3]=0.875*chroma[indx+u][c]+0.125*chroma[indx+w][c];				

				chroma[indx][c]=(f[0]*g[0]+f[1]*g[1]+f[2]*g[2]+f[3]*g[3])/(f[0]+f[1]+f[2]+f[3]);
			}

	for(row=3; row<height-3; row++)
		for(col=3,indx=row*width+col; col<width-3; col++,indx++){
			image[indx][0]=CLIP(chroma[indx][0]+image[indx][1]);
			image[indx][2]=CLIP(chroma[indx][1]+image[indx][1]);
		}

	free(chroma);
}





// green is used to create
// an interpolation direction map 
// 1 = vertical
// 0 = horizontal
// saved in image[][3]
void CLASS dcb_map()
{	
	int current, row, col, c, u=width, v=2*u, indx;

	for (row=2; row < height-2; row++) {
	for (col=2, indx=row*width+col; col < width-2; col++, indx++) { 

		if (image[indx][1] > ( image[indx-1][1] + image[indx+1][1] + image[indx-u][1] + image[indx+u][1])/4.0)
			image[indx][3] = ((MIN( image[indx-1][1], image[indx+1][1]) + image[indx-1][1] + image[indx+1][1] ) < (MIN( image[indx-u][1], image[indx+u][1]) + image[indx-u][1] + image[indx+u][1]));   
		else
			image[indx][3] = ((MAX( image[indx-1][1], image[indx+1][1]) + image[indx-1][1] + image[indx+1][1] ) > (MAX( image[indx-u][1], image[indx+u][1]) + image[indx-u][1] + image[indx+u][1])) ; 
	}
	}
}





// interpolated green pixels are corrected using the map
void CLASS dcb_correction()
{
	int current, row, col, c, u=width, v=2*u, indx;

	for (row=4; row < height-4; row++) {
	for (col=4, indx=row*width+col; col < width-4; col++, indx++) { 

		c =  FC(row,col);
	
		if (c != 1)
		{
			current = 4*image[indx][3] + 
				      2*(image[indx+u][3] + image[indx-u][3] + image[indx+1][3] + image[indx-1][3]) + 
					    image[indx+v][3] + image[indx-v][3] + image[indx+2][3] + image[indx-2][3];
						
			image[indx][1] = ((16-current)*(image[indx-1][1] + image[indx+1][1])/2.0 + current*(image[indx-u][1] + image[indx+u][1])/2.0)/16.0;		   
		}
	
	}
	}

}





// interpolated green pixels are corrected using the map
// with correction
void CLASS dcb_correction2()
{
	int current, row, col, c, u=width, v=2*u, indx;
	ushort (*pix)[4];

	for (row=4; row < height-4; row++) {
	for (col=4, indx=row*width+col; col < width-4; col++, indx++) { 

		c =  FC(row,col);
	
		if (c != 1)
		{
			current = 4*image[indx][3] + 
				      2*(image[indx+u][3] + image[indx-u][3] + image[indx+1][3] + image[indx-1][3]) + 
					    image[indx+v][3] + image[indx-v][3] + image[indx+2][3] + image[indx-2][3];
						
			image[indx][1] = CLIP(((16-current)*((image[indx-1][1] + image[indx+1][1])/2.0 + image[indx][c] - (image[indx+2][c] + image[indx-2][c])/2.0) + current*((image[indx-u][1] + image[indx+u][1])/2.0 + image[indx][c] - (image[indx+v][c] + image[indx-v][c])/2.0))/16.0);			   
		}
	
	}
	}

}






// image refinement
void CLASS dcb_refinement()
{
	int row, col, c, u=width, v=2*u, w=3*u, x=4*u, y=5*u, indx, max, min;
	float f[4], g[4];
	
	for (row=5; row < height-5; row++)
		for (col=5+(FC(row,1)&1),indx=row*width+col,c=FC(row,col); col < u-5; col+=2,indx+=2) {

// Cubic Spline Interpolation by Li and Randhawa, modified by Jacek Gozdz and Luis Sanz Rodríguez
		f[0]=1.0/(1.0+abs(image[indx-u][c]-image[indx][c])+abs(image[indx-u][1]-image[indx][1]));
		f[1]=1.0/(1.0+abs(image[indx+1][c]-image[indx][c])+abs(image[indx+1][1]-image[indx][1]));
		f[2]=1.0/(1.0+abs(image[indx-1][c]-image[indx][c])+abs(image[indx-1][1]-image[indx][1]));
		f[3]=1.0/(1.0+abs(image[indx+u][c]-image[indx][c])+abs(image[indx+u][1]-image[indx][1]));

g[0]=CLIP(image[indx-u][1]+0.5*(image[indx][c]-image[indx-u][c]) + 0.25*(image[indx][c]-image[indx-v][c]));
g[1]=CLIP(image[indx+1][1]+0.5*(image[indx][c]-image[indx+1][c]) + 0.25*(image[indx][c]-image[indx+2][c]));
g[2]=CLIP(image[indx-1][1]+0.5*(image[indx][c]-image[indx-1][c]) + 0.25*(image[indx][c]-image[indx-2][c]));
g[3]=CLIP(image[indx+u][1]+0.5*(image[indx][c]-image[indx+u][c]) + 0.25*(image[indx][c]-image[indx+v][c]));



	image[indx][1]=CLIP(((f[0]*g[0]+f[1]*g[1]+f[2]*g[2]+f[3]*g[3])/(f[0]+f[1]+f[2]+f[3]) ));

// get rid of the overshooted pixels
	min = MIN(image[indx+1+u][1], MIN(image[indx+1-u][1], MIN(image[indx-1+u][1], MIN(image[indx-1-u][1], MIN(image[indx-1][1], MIN(image[indx+1][1], MIN(image[indx-u][1], image[indx+u][1])))))));

	max = MAX(image[indx+1+u][1], MAX(image[indx+1-u][1], MAX(image[indx-1+u][1], MAX(image[indx-1-u][1], MAX(image[indx-1][1], MAX(image[indx+1][1], MAX(image[indx-u][1], image[indx+u][1])))))));

	image[indx][1] =  ULIM(image[indx][1], max, min);

			
		}
}





/*
image[indx][0] = CLIP(65536.0*(1.0 - (1.0-image[indx][0]/65536.0)*(1.0-image[indx][0]/65536.0)));
image[indx][1] = CLIP(65536.0*(1.0 - (1.0-image[indx][1]/65536.0)*(1.0-image[indx][1]/65536.0)));
image[indx][2] = CLIP(65536.0*(1.0 - (1.0-image[indx][2]/65536.0)*(1.0-image[indx][2]/65536.0)));
*/

// converts RGB to LCH colorspace and saves it to image3
void CLASS rgb_to_lch(double (*image3)[3])
{
	int indx;
	for (indx=0; indx < height*width; indx++) {

            image3[indx][0] = image[indx][0] + image[indx][1] + image[indx][2]; 		// L
            image3[indx][1] = 1.732050808 *(image[indx][0] - image[indx][1]);			// C
            image3[indx][2] = 2.0*image[indx][2] - image[indx][0] - image[indx][1];		// H
	}
}

// converts LCH to RGB colorspace and saves it back to image
void CLASS lch_to_rgb(double (*image3)[3])
{
	int indx;
	for (indx=0; indx < height*width; indx++) {

            image[indx][0] = CLIP(image3[indx][0] / 3.0 - image3[indx][2] / 6.0 + image3[indx][1] / 3.464101615);
            image[indx][1] = CLIP(image3[indx][0] / 3.0 - image3[indx][2] / 6.0 - image3[indx][1] / 3.464101615);
            image[indx][2] = CLIP(image3[indx][0] / 3.0 + image3[indx][2] / 3.0);
	}
}





// fast green interpolation
void CLASS fbdd_green2()
{
	int row, col, c, u=width, v=2*u, w=3*u, x=4*u, indx, current, current2, min, max, g1, g2;
	
	for (row=4; row < height-4; row++) {
	for (col=4, indx=row*width+col; col < width-4; col++, indx++) { 	

		c =  fc(row,col);
		if(c != 1) 
		{
			current = image[indx][c] - (image[indx+v][c] + image[indx-v][c] + image[indx-2][c] + image[indx+2][c])/4.0;
			
			g2 = (image[indx+u][1] + image[indx-u][1] + image[indx-1][1] + image[indx+1][1])/4.0;
			g1 = (image[indx+w][1] + image[indx-w][1] + image[indx-3][1] + image[indx+3][1])/4.0;
		
					image[indx][1] = CLIP((g2+g1)/2.0 + current);
					
min = MIN(image[indx-1][1], MIN(image[indx+1][1], MIN(image[indx-u][1], image[indx+u][1])));

max = MAX(image[indx-1][1], MAX(image[indx+1][1], MAX(image[indx-u][1], image[indx+u][1])));

	image[indx][1] =  ULIM(image[indx][1], max, min);					
			
		}
	} 
	}	
	
}

// denoising using interpolated neighbours
void CLASS fbdd_correction()
{
	int row, col, c, u=width, indx;
	ushort (*pix)[4];

	for (row=2; row < height-2; row++) {
	for (col=2, indx=row*width+col; col < width-2; col++, indx++) { 	

		c =  fc(row,col);

		image[indx][c] = ULIM(image[indx][c], 
					MAX(image[indx-1][c], MAX(image[indx+1][c], MAX(image[indx-u][c], image[indx+u][c]))), 
					MIN(image[indx-1][c], MIN(image[indx+1][c], MIN(image[indx-u][c], image[indx+u][c]))));

	} 
	}	
}

// corrects chroma noise
void CLASS fbdd_correction2(double (*image3)[3])
{
	int indx, u=width, v=2*width; 
double Co, Ho, ratio;
	for (indx=2+v; indx < height*width-(2+v); indx++) {

   if ( image3[indx][1]*image3[indx][2] != 0 ) {
	   		
            Co = (image3[indx+v][1] + image3[indx-v][1] + image3[indx-2][1] + image3[indx+2][1] - 
						MAX(image3[indx-2][1], MAX(image3[indx+2][1], MAX(image3[indx-v][1], image3[indx+v][1]))) -
						MIN(image3[indx-2][1], MIN(image3[indx+2][1], MIN(image3[indx-v][1], image3[indx+v][1]))))/2.0;
            Ho = (image3[indx+v][2] + image3[indx-v][2] + image3[indx-2][2] + image3[indx+2][2] - 
						MAX(image3[indx-2][2], MAX(image3[indx+2][2], MAX(image3[indx-v][2], image3[indx+v][2]))) -
						MIN(image3[indx-2][2], MIN(image3[indx+2][2], MIN(image3[indx-v][2], image3[indx+v][2]))))/2.0;
            ratio = sqrt ((Co*Co+Ho*Ho) / (image3[indx][1]*image3[indx][1] + image3[indx][2]*image3[indx][2]));
            
if (ratio < 0.85){
           image3[indx][1] = Co;
           image3[indx][2] = Ho;
			     }
  
	}
	}
}

// Cubic Spline Interpolation by Li and Randhawa, modified by Jacek Gozdz and Luis Sanz Rodríguez
void CLASS fbdd_green()
{
	int row, col, c, u=width, v=2*u, w=3*u, x=4*u, y=5*u, indx, min, max, current;
	float f[4], g[4];

	for (row=5; row < height-5; row++)
		for (col=5+(FC(row,1)&1),indx=row*width+col,c=FC(row,col); col < u-5; col+=2,indx+=2) {
			
		
f[0]=1.0/(1.0+abs(image[indx-u][1]-image[indx-w][1])+abs(image[indx-w][1]-image[indx+y][1]));
f[1]=1.0/(1.0+abs(image[indx+1][1]-image[indx+3][1])+abs(image[indx+3][1]-image[indx-5][1]));
f[2]=1.0/(1.0+abs(image[indx-1][1]-image[indx-3][1])+abs(image[indx-3][1]-image[indx+5][1]));
f[3]=1.0/(1.0+abs(image[indx+u][1]-image[indx+w][1])+abs(image[indx+w][1]-image[indx-y][1]));

g[0]=CLIP((23*image[indx-u][1]+23*image[indx-w][1]+2*image[indx-y][1]+8*(image[indx-v][c]-image[indx-x][c])+40*(image[indx][c]-image[indx-v][c]))/48.0);
g[1]=CLIP((23*image[indx+1][1]+23*image[indx+3][1]+2*image[indx+5][1]+8*(image[indx+2][c]-image[indx+4][c])+40*(image[indx][c]-image[indx+2][c]))/48.0);
g[2]=CLIP((23*image[indx-1][1]+23*image[indx-3][1]+2*image[indx-5][1]+8*(image[indx-2][c]-image[indx-4][c])+40*(image[indx][c]-image[indx-2][c]))/48.0);
g[3]=CLIP((23*image[indx+u][1]+23*image[indx+w][1]+2*image[indx+y][1]+8*(image[indx+v][c]-image[indx+x][c])+40*(image[indx][c]-image[indx+v][c]))/48.0);

	image[indx][1]=CLIP((f[0]*g[0]+f[1]*g[1]+f[2]*g[2]+f[3]*g[3])/(f[0]+f[1]+f[2]+f[3]));
	
	min = MIN(image[indx+1+u][1], MIN(image[indx+1-u][1], MIN(image[indx-1+u][1], MIN(image[indx-1-u][1], MIN(image[indx-1][1], MIN(image[indx+1][1], MIN(image[indx-u][1], image[indx+u][1])))))));

	max = MAX(image[indx+1+u][1], MAX(image[indx+1-u][1], MAX(image[indx-1+u][1], MAX(image[indx-1-u][1], MAX(image[indx-1][1], MAX(image[indx+1][1], MAX(image[indx-u][1], image[indx+u][1])))))));

	image[indx][1] = ULIM(image[indx][1], max, min);				
		}
}




// red and blue interpolation by Luis Sanz Rodríguez
void CLASS fbdd_color()
{
	int row,col,c,d,u=width,v=2*u,w=3*u,indx,(*chroma)[2];
	float f[4];
	chroma = (int (*)[2]) calloc(width*height,sizeof *chroma); merror (chroma, "fbdd_color2()");

	for (row=2; row < height-2; row++)
		for (col=2+(FC(row,2)&1),indx=row*width+col,c=FC(row,col),d=c/2; col<u-2; col+=2,indx+=2)
			chroma[indx][d]=image[indx][c]-image[indx][1];

for (row=3; row<height-3; row++)
	for (col=3+(FC(row,1)&1),indx=row*width+col,d=1-FC(row,col)/2,c=2*d; col<u-3; col+=2,indx+=2) {
		f[0]=1.0/(1.0+abs(chroma[indx-u-1][d]-chroma[indx+u+1][d])+abs(chroma[indx-u-1][d]-chroma[indx-w-3][d])+abs(chroma[indx+u+1][d]-chroma[indx-w-3][d]));
		f[1]=1.0/(1.0+abs(chroma[indx-u+1][d]-chroma[indx+u-1][d])+abs(chroma[indx-u+1][d]-chroma[indx-w+3][d])+abs(chroma[indx+u-1][d]-chroma[indx-w+3][d]));
		f[2]=1.0/(1.0+abs(chroma[indx+u-1][d]-chroma[indx-u+1][d])+abs(chroma[indx+u-1][d]-chroma[indx+w+3][d])+abs(chroma[indx-u+1][d]-chroma[indx+w-3][d]));
		f[3]=1.0/(1.0+abs(chroma[indx+u+1][d]-chroma[indx-u-1][d])+abs(chroma[indx+u+1][d]-chroma[indx+w-3][d])+abs(chroma[indx-u-1][d]-chroma[indx+w+3][d]));
		chroma[indx][d]=(f[0]*chroma[indx-u-1][d]+f[1]*chroma[indx-u+1][d]+f[2]*chroma[indx+u-1][d]+f[3]*chroma[indx+u+1][d])/(f[0]+f[1]+f[2]+f[3]);
		image[indx][c]=CLIP(chroma[indx][d]+image[indx][1]);
	}

for (row=3; row<height-3; row++)
	for (col=3+(FC(row,2)&1),indx=row*width+col; col<u-3; col+=2,indx+=2)
		for(c=d=0;d<=1;c+=2,d++){
			f[0]=1.0/(1.0+abs(chroma[indx-u][d]-chroma[indx+u][d])+abs(chroma[indx-u][d]-chroma[indx-w][d])+abs(chroma[indx+u][d]-chroma[indx-w][d]));
			f[1]=1.0/(1.0+abs(chroma[indx+1][d]-chroma[indx-1][d])+abs(chroma[indx+1][d]-chroma[indx+3][d])+abs(chroma[indx-1][d]-chroma[indx+3][d]));
			f[2]=1.0/(1.0+abs(chroma[indx-1][d]-chroma[indx+1][d])+abs(chroma[indx-1][d]-chroma[indx-3][d])+abs(chroma[indx+1][d]-chroma[indx-3][d]));
			f[3]=1.0/(1.0+abs(chroma[indx+u][d]-chroma[indx-u][d])+abs(chroma[indx+u][d]-chroma[indx+w][d])+abs(chroma[indx-u][d]-chroma[indx+w][d]));
			image[indx][c]=CLIP((f[0]*chroma[indx-u][d]+f[1]*chroma[indx+1][d]+f[2]*chroma[indx-1][d]+f[3]*chroma[indx+u][d])/(f[0]+f[1]+f[2]+f[3])+image[indx][1]);
		}

free(chroma);
}





// FBDD (Fake Before Demosaicing Denoising)
void CLASS fbdd(int noiserd)
{ 
	double (*image3)[3];
	image3 = (double (*)[3]) calloc(width*height, sizeof *image3);

	border_interpolate(4);

if (noiserd>1)
{
	if (verbose) fprintf (stderr,_("FBDD full noise reduction...\n"));	

	fbdd_green();
	fbdd_color();
	fbdd_correction();	

	dcb_color();
	rgb_to_lch(image3);
	fbdd_correction2(image3);
	fbdd_correction2(image3);
	lch_to_rgb(image3); 
	
	fbdd_green();
	fbdd_color();
	fbdd_correction();		
}
else
{
	if (verbose) fprintf (stderr,_("FBDD noise reduction...\n"));	
	fbdd_green();
	fbdd_color();
	fbdd_correction();	
}
	
}


// DCB demosaicing main routine (sharp version)
void CLASS dcb(int iterations, int dcb_enhance)
{


	int i=1;
	float (*image2)[3];
	image2 = (float (*)[3]) calloc(width*height, sizeof *image2);

	if (verbose) fprintf (stderr,_("DCB demosaicing...\n"));	
 
 		border_interpolate(2);
		copy_to_buffer(image2);

hid();
dcb_color();

		while (i<=iterations)
		{
			if (verbose) fprintf (stderr,_("DCB correction pass %d...\n"), i);
			hid2();
			hid2();
			hid2();
			dcb_map();
			dcb_correction();	
			i++;
		}
		
		dcb_color();
		dcb_pp();	
		hid2();
		hid2();  
		hid2();

	if (verbose) fprintf (stderr,_("finishing DCB...\n"));
		
		dcb_map();
		dcb_correction2();
			
		restore_from_buffer(image2); 
			
		dcb_map();
		dcb_correction();

		dcb_color();
		dcb_pp();
		dcb_map();
		dcb_correction();

		dcb_map();
		dcb_correction();	

		restore_from_buffer(image2);
		dcb_color();
						
	if (dcb_enhance)
	{
		if (verbose) fprintf (stderr,_("optional DCB refinement...\n"));			
		dcb_refinement();
		dcb_color_full();
	}


		free(image2);

}

