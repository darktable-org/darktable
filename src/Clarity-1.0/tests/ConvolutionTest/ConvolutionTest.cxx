/* 
 * Clarity is Copyright 2008 Center for Integrated Systems for Microscopy, 
 * Copyright 2008 University of North Carolina at Chapel Hill.
 *
 * Clarity is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Public License as published by the Free Software 
 * Foundation; either version 2 of the License, or (at your option) any 
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA. You can also find 
 * the GPL on the GNU web site (http://www.gnu.org/copyleft/gpl.html).
 *
 * File name: ConvolutionTest.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include <cstdio>

#include "Clarity.h"

#include "../common/Test_Common.h"


int main(int argc, char* argv[]) {
  float *inputImage, *kernelImage;
  Clarity_Dim3 imageDims; // Image dimensions
  Clarity_Dim3 kernelDims; // Kernel dimensions

  // Initialize image data arrays.
  inputImage  = Test_GenerateTrueImage(&imageDims);
  kernelImage = Test_GenerateGaussianKernel(&kernelDims, 3.0f);

  // Write image and PSF to files.
  FILE *fp = fopen("image_f32.raw", "wb");
  fwrite(inputImage, sizeof(float), imageDims.x*imageDims.y*imageDims.z, fp);
  fclose(fp);

  fp = fopen("psf_f32.raw", "wb");
  fwrite(kernelImage, sizeof(float), kernelDims.x*kernelDims.y*kernelDims.z, fp);
  fclose(fp);

  // Initialize Clarity by registering this application as a client.
  Clarity_Register();

  // We'll create test data here by convolving the input image with the PSF.
  float *convolvedImage = 
    (float *) malloc(sizeof(float)*imageDims.x*imageDims.y*imageDims.z);
  Clarity_Convolve(inputImage, imageDims, kernelImage, kernelDims,
		   convolvedImage);

  // Write out convolved image.
  fp = fopen("convolved_f32.raw", "wb");
  fwrite(convolvedImage, sizeof(float), imageDims.x*imageDims.y*imageDims.z, fp);
  fclose(fp);

  // Unregister
  Clarity_UnRegister();

  free(inputImage);
  free(kernelImage);
  free(convolvedImage);

  return 0;
}
